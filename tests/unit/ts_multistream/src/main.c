/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Concurrent time-series streams against the real LittleFS backend, using the
 * fstab automounted /lfs filesystem provided by boards/native_sim.overlay.
 *
 * Built with CONFIG_ZDB_TS_MAX_STREAMS=4 so several streams can be open at
 * once. Each test uses its own stream names so files do not interact.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "zephyrdb.h"

#define MAX_STREAMS CONFIG_ZDB_TS_MAX_STREAMS

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void ms_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void ms_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(ts_multistream, NULL, NULL, ms_before, ms_after, NULL);

static void append_one(zdb_ts_t *ts, uint64_t ts_ms, int64_t value)
{
	zdb_ts_sample_i64_t sample = {.ts_ms = ts_ms, .value = value};

	zassert_equal(zdb_ts_append_i64(ts, &sample), ZDB_OK, "append failed");
}

static uint32_t count_stream(zdb_ts_t *ts)
{
	zdb_ts_agg_result_t result = {0};

	zassert_equal(zdb_ts_query_aggregate(ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result),
		      ZDB_OK, "count failed");
	return result.points;
}

/* Samples written to one stream must not land in another. */
ZTEST(ts_multistream, test_interleaved_streams_stay_separate)
{
	zdb_ts_t a;
	zdb_ts_t b;
	zdb_ts_t c;

	zassert_equal(zdb_ts_open(&g_db, "ms_a", &a), ZDB_OK, "open a failed");
	zassert_equal(zdb_ts_open(&g_db, "ms_b", &b), ZDB_OK, "open b failed");
	zassert_equal(zdb_ts_open(&g_db, "ms_c", &c), ZDB_OK, "open c failed");

	/* Interleave so a shared buffer would mix them up. */
	append_one(&a, 1000U, 1);
	append_one(&b, 1000U, 2);
	append_one(&c, 1000U, 3);
	append_one(&a, 1001U, 4);
	append_one(&b, 1001U, 5);

	zassert_equal(zdb_ts_flush_sync(&a, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(count_stream(&a), 2U, "stream a has the wrong count");
	zassert_equal(count_stream(&b), 2U, "stream b has the wrong count");
	zassert_equal(count_stream(&c), 1U, "stream c has the wrong count");

	(void)zdb_ts_close(&a);
	(void)zdb_ts_close(&b);
	(void)zdb_ts_close(&c);
}

/* One flush must persist every stream's buffered samples, not just one. */
ZTEST(ts_multistream, test_flush_covers_all_streams)
{
	zdb_ts_t a;
	zdb_ts_t b;

	zassert_equal(zdb_ts_open(&g_db, "ms_f_a", &a), ZDB_OK, "open a failed");
	zassert_equal(zdb_ts_open(&g_db, "ms_f_b", &b), ZDB_OK, "open b failed");

	append_one(&a, 2000U, 10);
	append_one(&b, 2000U, 20);

	zassert_equal(zdb_ts_flush_sync(&a, K_SECONDS(2)), ZDB_OK, "flush failed");

	/* Close and reopen so only persisted records can be counted. */
	(void)zdb_ts_close(&a);
	(void)zdb_ts_close(&b);
	(void)zdb_deinit(&g_db);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "re-init failed");

	zassert_equal(zdb_ts_open(&g_db, "ms_f_a", &a), ZDB_OK, "reopen a failed");
	zassert_equal(zdb_ts_open(&g_db, "ms_f_b", &b), ZDB_OK, "reopen b failed");
	zassert_equal(count_stream(&a), 1U, "stream a was not flushed");
	zassert_equal(count_stream(&b), 1U, "stream b was not flushed");

	(void)zdb_ts_close(&a);
	(void)zdb_ts_close(&b);
}

/* Opening more streams than slots reports BUSY rather than evicting one. */
ZTEST(ts_multistream, test_exhausting_slots_reports_busy)
{
	zdb_ts_t open_streams[MAX_STREAMS];
	zdb_ts_t overflow;
	char name[16];

	for (size_t i = 0U; i < MAX_STREAMS; i++) {
		(void)snprintk(name, sizeof(name), "ms_slot%u", (unsigned)i);
		zassert_equal(zdb_ts_open(&g_db, name, &open_streams[i]), ZDB_OK,
			      "open %zu failed", i);
	}

	zassert_equal(zdb_ts_open(&g_db, "ms_slot_over", &overflow), ZDB_ERR_BUSY,
		      "expected BUSY once every slot is taken");

	for (size_t i = 0U; i < MAX_STREAMS; i++) {
		(void)zdb_ts_close(&open_streams[i]);
	}
}

/* Closing a stream frees its slot for a different one. */
ZTEST(ts_multistream, test_close_releases_slot)
{
	zdb_ts_t open_streams[MAX_STREAMS];
	zdb_ts_t late;
	char name[16];

	for (size_t i = 0U; i < MAX_STREAMS; i++) {
		(void)snprintk(name, sizeof(name), "ms_rel%u", (unsigned)i);
		zassert_equal(zdb_ts_open(&g_db, name, &open_streams[i]), ZDB_OK,
			      "open %zu failed", i);
	}

	zassert_equal(zdb_ts_close(&open_streams[0]), ZDB_OK, "close failed");

	zassert_equal(zdb_ts_open(&g_db, "ms_rel_late", &late), ZDB_OK,
		      "slot not released by close");

	(void)zdb_ts_close(&late);
	for (size_t i = 1U; i < MAX_STREAMS; i++) {
		(void)zdb_ts_close(&open_streams[i]);
	}
}

/* Closing must persist buffered samples rather than discard them. */
ZTEST(ts_multistream, test_close_flushes_buffered_samples)
{
	zdb_ts_t ts;

	zassert_equal(zdb_ts_open(&g_db, "ms_closeflush", &ts), ZDB_OK, "open failed");
	append_one(&ts, 3000U, 42);
	/* No explicit flush: closing must not lose the sample. */
	zassert_equal(zdb_ts_close(&ts), ZDB_OK, "close failed");

	(void)zdb_deinit(&g_db);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "re-init failed");

	zassert_equal(zdb_ts_open(&g_db, "ms_closeflush", &ts), ZDB_OK, "reopen failed");
	zassert_equal(count_stream(&ts), 1U, "buffered sample lost on close");
	(void)zdb_ts_close(&ts);
}

/*
 * Re-opening a stream shares its slot, so closing one handle must not pull the
 * buffer out from under the other.
 */
ZTEST(ts_multistream, test_second_handle_survives_first_close)
{
	zdb_ts_t first;
	zdb_ts_t second;

	zassert_equal(zdb_ts_open(&g_db, "ms_shared", &first), ZDB_OK, "first open failed");
	zassert_equal(zdb_ts_open(&g_db, "ms_shared", &second), ZDB_OK, "second open failed");

	append_one(&first, 4000U, 7);
	zassert_equal(zdb_ts_close(&first), ZDB_OK, "first close failed");

	/* The surviving handle can still append and read its buffered sample. */
	append_one(&second, 4001U, 8);
	zassert_equal(count_stream(&second), 2U, "shared slot lost samples");

	zassert_equal(zdb_ts_close(&second), ZDB_OK, "second close failed");
}

/* Cursors on different streams must not observe each other's records. */
ZTEST(ts_multistream, test_concurrent_cursors_are_independent)
{
	zdb_ts_t a;
	zdb_ts_t b;
	zdb_cursor_t cursor_a;
	zdb_cursor_t cursor_b;
	zdb_bytes_t record;
	size_t count_a = 0U;
	size_t count_b = 0U;

	zassert_equal(zdb_ts_open(&g_db, "ms_cur_a", &a), ZDB_OK, "open a failed");
	zassert_equal(zdb_ts_open(&g_db, "ms_cur_b", &b), ZDB_OK, "open b failed");

	append_one(&a, 5000U, 1);
	append_one(&a, 5001U, 2);
	append_one(&b, 5000U, 3);
	zassert_equal(zdb_ts_flush_sync(&a, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_cursor_open(&a, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor_a), ZDB_OK,
		      "cursor a failed");
	zassert_equal(zdb_ts_cursor_open(&b, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor_b), ZDB_OK,
		      "cursor b failed");

	/* Interleave the walks to catch any shared iteration state. */
	while (zdb_cursor_next(&cursor_a, &record) == ZDB_OK) {
		count_a++;
		if (zdb_cursor_next(&cursor_b, &record) == ZDB_OK) {
			count_b++;
		}
	}
	while (zdb_cursor_next(&cursor_b, &record) == ZDB_OK) {
		count_b++;
	}

	zassert_equal(count_a, 2U, "cursor a saw %zu records", count_a);
	zassert_equal(count_b, 1U, "cursor b saw %zu records", count_b);

	(void)zdb_cursor_close(&cursor_a);
	(void)zdb_cursor_close(&cursor_b);
	(void)zdb_ts_close(&a);
	(void)zdb_ts_close(&b);
}

/* Watermarks are per stream. */
ZTEST(ts_multistream, test_watermarks_are_per_stream)
{
	zdb_ts_t a;
	zdb_ts_t b;
	uint64_t mark = 0U;

	zassert_equal(zdb_ts_open(&g_db, "ms_wm_a", &a), ZDB_OK, "open a failed");
	zassert_equal(zdb_ts_open(&g_db, "ms_wm_b", &b), ZDB_OK, "open b failed");

	zassert_equal(zdb_ts_watermark_set(&a, 111U), ZDB_OK, "set a failed");
	zassert_equal(zdb_ts_watermark_set(&b, 222U), ZDB_OK, "set b failed");

	zassert_equal(zdb_ts_watermark_get(&a, &mark), ZDB_OK, "get a failed");
	zassert_equal(mark, 111U, "stream a watermark mismatch: %llu", (unsigned long long)mark);
	zassert_equal(zdb_ts_watermark_get(&b, &mark), ZDB_OK, "get b failed");
	zassert_equal(mark, 222U, "stream b watermark mismatch: %llu", (unsigned long long)mark);

	(void)zdb_ts_close(&a);
	(void)zdb_ts_close(&b);
}
