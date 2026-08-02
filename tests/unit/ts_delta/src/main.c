/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compact (delta-encoded) time-series records against the real LittleFS
 * backend, using the fstab automounted /lfs filesystem from
 * boards/native_sim.overlay.
 *
 * Built with CONFIG_ZDB_TS_DELTA_ENCODING=y, so records are 16 bytes and
 * timestamps are offsets from a base in each segment's header.
 *
 * Each test uses its own stream name so segment files do not interact.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "zephyrdb.h"

#define V1_RECORD_BYTES 28U
#define V2_RECORD_BYTES 16U

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void delta_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void delta_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(ts_delta, NULL, NULL, delta_before, delta_after, NULL);

static void append_ramp(zdb_ts_t *ts, size_t count, uint64_t first_ts_ms)
{
	for (size_t i = 0U; i < count; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = first_ts_ms + i,
			.value = (int64_t)i,
		};

		zassert_equal(zdb_ts_append_i64(ts, &sample), ZDB_OK, "append %zu failed", i);
	}
}

static uint32_t count_all(zdb_ts_t *ts)
{
	zdb_ts_agg_result_t result = {0};

	zassert_equal(zdb_ts_query_aggregate(ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result),
		      ZDB_OK, "count failed");
	return result.points;
}

/* Timestamps and values must survive the offset encoding exactly. */
ZTEST(ts_delta, test_roundtrip_preserves_timestamps_and_values)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const uint64_t first = 1000U;
	const size_t total = 5U;
	size_t seen = 0U;

	zassert_equal(zdb_ts_open(&g_db, "d_rt", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total, first);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	/* Windows filter on decoded timestamps, so an exact window proves them. */
	for (size_t i = 0U; i < total; i++) {
		zdb_ts_window_t one = {.from_ts_ms = first + i, .to_ts_ms = first + i};
		zdb_ts_agg_result_t result = {0};

		zassert_equal(zdb_ts_query_aggregate(&ts, one, ZDB_TS_AGG_SUM, &result), ZDB_OK,
			      "DBG sample %zu ts=%llu not found", i, (unsigned long long)(first + i));
		zassert_equal(result.points, 1U, "sample %zu matched %u records", i,
			      result.points);
		zassert_within(result.value, (double)i, 0.001, "sample %zu value wrong", i);
	}

	zassert_equal(zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		zassert_equal(record.len, V2_RECORD_BYTES, "record is %zu bytes, expected %u",
			      record.len, V2_RECORD_BYTES);
		seen++;
	}
	(void)zdb_cursor_close(&cursor);
	zassert_equal(seen, total, "walk saw %zu of %zu", seen, total);

	(void)zdb_ts_close(&ts);
}

/* The point of the feature: the same samples take less storage. */
ZTEST(ts_delta, test_records_are_smaller_on_storage)
{
	zdb_ts_t ts;
	struct fs_dirent info;
	const size_t total = 10U;
	size_t payload;

	zassert_equal(zdb_ts_open(&g_db, "d_size", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total, 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");
	(void)zdb_ts_close(&ts);

	zassert_equal(fs_stat("/lfs/zdb/d_size.0000.zts", &info), 0, "segment not found");

	/* 24-byte compact header, then one 16-byte record per sample. */
	zassert_true((size_t)info.size > 24U, "segment holds no records");
	payload = (size_t)info.size - 24U;
	zassert_equal(payload, total * V2_RECORD_BYTES, "payload is %zu bytes, expected %zu",
		      payload, total * V2_RECORD_BYTES);
	zassert_true(payload < (total * V1_RECORD_BYTES), "compact records are not smaller");
}

/* COUNT reads sizes, so it has to divide by the right record size. */
ZTEST(ts_delta, test_count_matches_cursor)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const size_t total = 12U;
	size_t walked = 0U;

	zassert_equal(zdb_ts_open(&g_db, "d_count", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total, 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		walked++;
	}
	(void)zdb_cursor_close(&cursor);

	zassert_equal((size_t)count_all(&ts), walked, "count disagrees with walk (%zu)", walked);
	zassert_equal(walked, total, "walk saw %zu of %zu", walked, total);

	(void)zdb_ts_close(&ts);
}

/* Reverse traversal depends on a fixed stride, which compact records keep. */
ZTEST(ts_delta, test_descending_cursor_reverses_order)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const size_t total = 6U;
	int64_t previous = INT64_MAX;
	size_t seen = 0U;

	zassert_equal(zdb_ts_open(&g_db, "d_desc", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total, 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_cursor_open_desc(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor),
		      ZDB_OK, "descending cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		struct {
			uint32_t ts_delta_ms_le;
			uint64_t value_le;
			uint32_t crc_le;
		} __packed rec;
		int64_t value;

		zassert_equal(record.len, sizeof(rec), "DBG record.len=%zu expected=%zu", record.len, sizeof(rec));
		(void)memcpy(&rec, record.data, sizeof(rec));
		value = (int64_t)sys_le64_to_cpu(rec.value_le);
		zassert_true(value < previous, "out of order: %lld after %lld", (long long)value,
			     (long long)previous);
		previous = value;
		seen++;
	}
	(void)zdb_cursor_close(&cursor);
	zassert_equal(seen, total, "reverse walk saw %zu of %zu", seen, total);

	(void)zdb_ts_close(&ts);
}

/* Unflushed samples decode the same way as stored ones. */
ZTEST(ts_delta, test_unflushed_samples_decode)
{
	zdb_ts_t ts;
	zdb_ts_window_t one = {.from_ts_ms = 2001U, .to_ts_ms = 2001U};
	zdb_ts_agg_result_t result = {0};

	zassert_equal(zdb_ts_open(&g_db, "d_ram", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, 3U, 2000U);

	zassert_equal(count_all(&ts), 3U, "buffered samples not counted");
	zassert_equal(zdb_ts_query_aggregate(&ts, one, ZDB_TS_AGG_SUM, &result), ZDB_OK,
		      "buffered sample not found at its timestamp");
	zassert_within(result.value, 1.0, 0.001, "buffered value wrong");

	(void)zdb_ts_close(&ts);
}

/* Data survives a restart, which is where the header's base is re-read. */
ZTEST(ts_delta, test_survives_reinit)
{
	zdb_ts_t ts;
	zdb_ts_window_t one = {.from_ts_ms = 3002U, .to_ts_ms = 3002U};
	zdb_ts_agg_result_t result = {0};

	zassert_equal(zdb_ts_open(&g_db, "d_boot", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, 4U, 3000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");
	(void)zdb_ts_close(&ts);

	(void)zdb_deinit(&g_db);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "re-init failed");

	zassert_equal(zdb_ts_open(&g_db, "d_boot", &ts), ZDB_OK, "reopen failed");
	zassert_equal(count_all(&ts), 4U, "records lost across restart");
	zassert_equal(zdb_ts_query_aggregate(&ts, one, ZDB_TS_AGG_SUM, &result), ZDB_OK,
		      "timestamp not preserved across restart");
	zassert_within(result.value, 2.0, 0.001, "value wrong after restart");

	(void)zdb_ts_close(&ts);
}

/*
 * A timestamp too far past the segment's base cannot be expressed as a 32-bit
 * offset, so the stream starts a segment based on that sample rather than
 * failing the append.
 */
ZTEST(ts_delta, test_far_future_timestamp_rebases)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t far = {.ts_ms = 1000ULL + 0x1FFFFFFFFULL, .value = 99};
	zdb_ts_window_t far_window = {.from_ts_ms = far.ts_ms, .to_ts_ms = far.ts_ms};
	zdb_ts_agg_result_t result = {0};

	zassert_equal(zdb_ts_open(&g_db, "d_far", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, 2U, 1000U);

	zassert_equal(zdb_ts_append_i64(&ts, &far), ZDB_OK, "far-future append rejected");
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	/* Both the old samples and the far one are readable. */
	zassert_equal(count_all(&ts), 3U, "samples lost across the rebase");
	zassert_equal(zdb_ts_query_aggregate(&ts, far_window, ZDB_TS_AGG_SUM, &result), ZDB_OK,
		      "far-future sample not found at its timestamp");
	zassert_within(result.value, 99.0, 0.001, "far-future value wrong");

	(void)zdb_ts_close(&ts);
}
