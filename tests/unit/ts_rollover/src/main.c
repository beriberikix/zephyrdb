/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bounded (rolling) time-series streams against the real LittleFS backend,
 * using the fstab automounted /lfs filesystem from boards/native_sim.overlay.
 *
 * Built with 280-byte segments (10 records) and 3 retained segments, so a
 * stream reaches its bound after ~30 records and starts discarding the oldest.
 *
 * Each test uses its own stream name so segment files do not interact.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <string.h>

#include "zephyrdb.h"

#define RECORD_BYTES  28U
#define SEG_RECORDS   (CONFIG_ZDB_TS_ROLLOVER_SEGMENT_BYTES / RECORD_BYTES)
#define MAX_SEGMENTS  CONFIG_ZDB_TS_ROLLOVER_MAX_SEGMENTS

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void ro_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void ro_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(ts_rollover, NULL, NULL, ro_before, ro_after, NULL);

/* Append @p count samples with value == index, timestamps from 1000. */
static void append_ramp(zdb_ts_t *ts, size_t count)
{
	for (size_t i = 0U; i < count; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = 1000U + i,
			.value = (int64_t)i,
		};

		zassert_equal(zdb_ts_append_i64(ts, &sample), ZDB_OK, "append %zu failed", i);
	}
}

/* Matches the stream-id hash the library stores in a stream header. */
static uint32_t fnv1a32(const char *name)
{
	uint32_t hash = 0x811C9DC5u;

	while (*name != '\0') {
		hash ^= (uint8_t)(*name);
		hash *= 0x01000193u;
		name++;
	}

	return hash;
}

static int64_t record_value(const zdb_bytes_t *record)
{
	struct {
		uint32_t magic_le;
		uint16_t version_le;
		uint16_t reserved_le;
		uint64_t ts_ms_le;
		uint64_t value_le;
		uint32_t crc_le;
	} __packed rec;

	zassert_equal(record->len, sizeof(rec), "unexpected record size");
	(void)memcpy(&rec, record->data, sizeof(rec));
	return (int64_t)sys_le64_to_cpu(rec.value_le);
}

static uint32_t count_all(zdb_ts_t *ts)
{
	zdb_ts_agg_result_t result = {0};

	zassert_equal(zdb_ts_query_aggregate(ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result),
		      ZDB_OK, "count failed");
	return result.points;
}

/*
 * The point of rollover: a stream that keeps being written stays bounded, and
 * what it keeps is the most recent data.
 */
ZTEST(ts_rollover, test_stream_stays_bounded_and_keeps_newest)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const size_t total = SEG_RECORDS * (MAX_SEGMENTS + 2U);
	int64_t newest = -1;
	uint32_t count;

	zassert_equal(zdb_ts_open(&g_db, "ro_bound", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	count = count_all(&ts);
	zassert_true(count < total, "stream did not roll: %u of %zu retained", count,
		     total);
	zassert_true(count >= SEG_RECORDS, "stream discarded too much: %u", count);

	/* The most recent sample must still be there. */
	zassert_equal(zdb_ts_cursor_open_desc(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor),
		      ZDB_OK, "descending cursor failed");
	if (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		newest = record_value(&record);
	}
	(void)zdb_cursor_close(&cursor);

	zassert_equal(newest, (int64_t)(total - 1U), "newest sample lost: %lld",
		      (long long)newest);

	(void)zdb_ts_close(&ts);
}

/* A forward walk must cross segment boundaries and stay in order. */
ZTEST(ts_rollover, test_cursor_spans_segments_in_order)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const size_t total = SEG_RECORDS * 2U + 3U;
	int64_t previous = INT64_MIN;
	size_t seen = 0U;

	zassert_equal(zdb_ts_open(&g_db, "ro_span", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		int64_t value = record_value(&record);

		zassert_true(value > previous, "out of order across segments: %lld after %lld",
			     (long long)value, (long long)previous);
		previous = value;
		seen++;
	}
	(void)zdb_cursor_close(&cursor);

	zassert_equal(seen, total, "walk missed records across segments: %zu of %zu", seen,
		      total);

	(void)zdb_ts_close(&ts);
}

/* And the reverse walk must do the same, newest to oldest. */
ZTEST(ts_rollover, test_descending_cursor_spans_segments)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const size_t total = SEG_RECORDS * 2U + 3U;
	int64_t previous = INT64_MAX;
	size_t seen = 0U;

	zassert_equal(zdb_ts_open(&g_db, "ro_span_desc", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_cursor_open_desc(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor),
		      ZDB_OK, "descending cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		int64_t value = record_value(&record);

		zassert_true(value < previous, "out of order in reverse: %lld after %lld",
			     (long long)value, (long long)previous);
		previous = value;
		seen++;
	}
	(void)zdb_cursor_close(&cursor);

	zassert_equal(seen, total, "reverse walk missed records: %zu of %zu", seen, total);

	(void)zdb_ts_close(&ts);
}

/* COUNT reads sizes rather than payloads, so it must add up every segment. */
ZTEST(ts_rollover, test_count_matches_cursor_across_segments)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const size_t total = SEG_RECORDS * 2U + 5U;
	size_t walked = 0U;
	uint32_t counted;

	zassert_equal(zdb_ts_open(&g_db, "ro_count", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	counted = count_all(&ts);

	zassert_equal(zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		walked++;
	}
	(void)zdb_cursor_close(&cursor);

	zassert_equal((size_t)counted, walked, "count %u disagrees with walk %zu", counted,
		      walked);

	(void)zdb_ts_close(&ts);
}

/* Buffered samples are part of the stream even before a flush. */
ZTEST(ts_rollover, test_count_includes_unflushed)
{
	zdb_ts_t ts;

	zassert_equal(zdb_ts_open(&g_db, "ro_ram", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, 3U);
	zassert_equal(count_all(&ts), 3U, "unflushed samples not counted");
	(void)zdb_ts_close(&ts);
}

/* Retained data must survive a restart, including the segment window. */
ZTEST(ts_rollover, test_segments_survive_reinit)
{
	zdb_ts_t ts;
	const size_t total = SEG_RECORDS * 2U;
	uint32_t before;
	uint32_t after;

	zassert_equal(zdb_ts_open(&g_db, "ro_boot", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");
	before = count_all(&ts);
	(void)zdb_ts_close(&ts);

	(void)zdb_deinit(&g_db);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "re-init failed");

	zassert_equal(zdb_ts_open(&g_db, "ro_boot", &ts), ZDB_OK, "reopen failed");
	after = count_all(&ts);
	zassert_equal(after, before, "record count changed across restart: %u then %u", before,
		      after);
	(void)zdb_ts_close(&ts);
}

/*
 * A stream written by a build without rollover is a single <stream>.zts file.
 * Enabling rollover must adopt it rather than strand its data.
 */
ZTEST(ts_rollover, test_adopts_single_file_stream)
{
	zdb_ts_t ts;
	struct fs_file_t file;
	struct zdb_ts_stream_header {
		uint32_t magic_le;
		uint16_t version_le;
		uint16_t reserved_le;
		uint32_t stream_id_le;
		uint32_t crc_le;
	} __packed hdr;
	struct fs_dirent info;
	int fs_rc;

	/* Make sure the stream directory exists before writing into it. */
	{
		zdb_ts_t seed;

		zassert_equal(zdb_ts_open(&g_db, "ro_seed", &seed), ZDB_OK, "seed open failed");
		(void)zdb_ts_close(&seed);
	}

	/*
	 * Build the legacy layout by hand: one <stream>.zts holding a valid
	 * stream header and no records, which is what a build without rollover
	 * writes.
	 */
	hdr.magic_le = sys_cpu_to_le32(0x5A445453u);
	hdr.version_le = sys_cpu_to_le16(1U);
	hdr.reserved_le = 0U;
	hdr.stream_id_le = sys_cpu_to_le32(fnv1a32("ro_legacy"));
	hdr.crc_le = sys_cpu_to_le32(
		crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.crc_le)));

	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/zdb/ro_legacy.zts", FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	zassert_equal(fs_rc, 0, "creating legacy stream failed: %d", fs_rc);
	zassert_equal(fs_write(&file, &hdr, sizeof(hdr)), (int)sizeof(hdr), "header write failed");
	zassert_equal(fs_close(&file), 0, "close failed");

	zassert_equal(zdb_ts_open(&g_db, "ro_legacy", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, 2U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	/* The single file is now segment 0, and the old name is gone. */
	zassert_equal(fs_stat("/lfs/zdb/ro_legacy.0000.zts", &info), 0,
		      "legacy stream not adopted as segment 0");
	zassert_not_equal(fs_stat("/lfs/zdb/ro_legacy.zts", &info), 0,
			  "legacy file left behind alongside its segment");

	(void)zdb_ts_close(&ts);
}

/* Watermarks are timestamps, so they stay usable after data ages out. */
ZTEST(ts_rollover, test_watermark_survives_discarded_range)
{
	zdb_ts_t ts;
	zdb_ts_window_t window = ZDB_TS_WINDOW_ALL;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	uint64_t mark = 0U;
	size_t remaining = 0U;

	zassert_equal(zdb_ts_open(&g_db, "ro_wm", &ts), ZDB_OK, "open failed");

	/* Acknowledge early samples, then write enough to discard them. */
	append_ramp(&ts, SEG_RECORDS);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");
	zassert_equal(zdb_ts_watermark_set(&ts, 1000U + 2U), ZDB_OK, "watermark set failed");

	append_ramp(&ts, SEG_RECORDS * (MAX_SEGMENTS + 1U));
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	/* The mark still reads back and still bounds a resumed drain. */
	zassert_equal(zdb_ts_watermark_get(&ts, &mark), ZDB_OK, "watermark get failed");
	zassert_equal(mark, 1002U, "watermark changed: %llu", (unsigned long long)mark);

	window.from_ts_ms = mark + 1U;
	zassert_equal(zdb_ts_cursor_open(&ts, window, NULL, NULL, &cursor), ZDB_OK,
		      "cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		remaining++;
	}
	(void)zdb_cursor_close(&cursor);

	zassert_equal(remaining, count_all(&ts),
		      "resumed drain disagrees with retained count: %zu", remaining);

	(void)zdb_ts_close(&ts);
}
