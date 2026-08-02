/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * TS unit tests against the real LittleFS backend, using the fstab
 * automounted /lfs filesystem provided by boards/native_sim.overlay.
 *
 * Each test runs on a fresh zdb instance (the TS context pins one active
 * stream per instance until deinit) and uses its own stream name so
 * stream files do not interact across tests.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include "zephyrdb.h"

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void ts_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void ts_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(ts_suite, NULL, NULL, ts_before, ts_after, NULL);

static void ts_append_fixed(zdb_ts_t *ts, const int64_t *values, size_t count,
			    uint64_t first_ts_ms)
{
	for (size_t i = 0U; i < count; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = first_ts_ms + i,
			.value = values[i],
		};
		zdb_status_t rc = zdb_ts_append_i64(ts, &sample);

		zassert_equal(rc, ZDB_OK, "append %zu failed: %d", i, rc);
	}
}

/*
 * cfg.work_q == NULL is documented to mean "use the system work queue"; async
 * flush used to reject it with ZDB_ERR_UNSUPPORTED instead.
 */
ZTEST(ts_suite, test_ts_flush_async_without_configured_work_q)
{
	static const zdb_cfg_t no_wq_cfg = {
		.kv_backend_fs = NULL,
		.lfs_mount_point = "/lfs",
		.work_q = NULL,
	};
	ZDB_DEFINE_STATIC(no_wq_db, no_wq_cfg);
	zdb_ts_t ts;
	int64_t values[] = {7, 8, 9};
	zdb_status_t rc;

	/* This suite's fixture already initialized g_db; use a separate instance. */
	zassert_equal(zdb_init(&no_wq_db, &no_wq_cfg), ZDB_OK, "init failed");

	rc = zdb_ts_open(&no_wq_db, "t_nowq", &ts);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);

	rc = zdb_ts_flush_async(&ts);
	zassert_equal(rc, ZDB_OK, "async flush rejected without cfg.work_q: %d", rc);

	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "sync flush failed: %d", rc);

	(void)zdb_ts_close(&ts);
	(void)zdb_deinit(&no_wq_db);
}

ZTEST(ts_suite, test_ts_open_close_success)
{
	zdb_ts_t ts;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_open", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);
	rc = zdb_ts_close(&ts);
	zassert_equal(rc, ZDB_OK, "close failed: %d", rc);
}

ZTEST(ts_suite, test_ts_open_invalid_args)
{
	zdb_ts_t ts;

	zassert_equal(zdb_ts_open(NULL, "s", &ts), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_open(&g_db, NULL, &ts), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_open(&g_db, "s", NULL), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_open(&g_db, "bad/name", &ts), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_open(&g_db, "..", &ts), ZDB_ERR_INVAL);
}

ZTEST(ts_suite, test_ts_append_single)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 100 };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_appone", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "append failed: %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_append_batch_api)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t samples[4];
	zdb_ts_agg_result_t result = {0};
	zdb_status_t rc = zdb_ts_open(&g_db, "t_batch", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	for (size_t i = 0U; i < ARRAY_SIZE(samples); i++) {
		samples[i].ts_ms = 2000U + i;
		samples[i].value = (int64_t)(10 * (i + 1U));
	}

	rc = zdb_ts_append_batch_i64(&ts, samples, ARRAY_SIZE(samples));
	zassert_equal(rc, ZDB_OK, "batch append failed: %d", rc);

	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "count aggregate failed: %d", rc);
	zassert_equal(result.points, 4U, "expected 4 points, got %u", result.points);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_flush_sync_success)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 100 };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_flush", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "append failed: %d", rc);

	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_flush_no_wait)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 100 };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_tmo", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "append failed: %d", rc);

	/*
	 * K_NO_WAIT may or may not observe the work completion in time.
	 * k_sem_take(K_NO_WAIT) reports -EBUSY rather than -EAGAIN, so an
	 * incomplete flush surfaces as BUSY instead of TIMEOUT.
	 */
	rc = zdb_ts_flush_sync(&ts, K_NO_WAIT);
	zassert_true((rc == ZDB_OK) || (rc == ZDB_ERR_TIMEOUT) || (rc == ZDB_ERR_BUSY),
		     "expected OK, TIMEOUT, or BUSY, got %d", rc);

	/* A patient flush afterwards must always succeed. */
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "follow-up flush failed: %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_query_aggregate_avg)
{
	zdb_ts_t ts;
	const int64_t values[] = {10, 20, 30};
	zdb_ts_agg_result_t result = {0};
	zdb_status_t rc = zdb_ts_open(&g_db, "t_avg", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_AVG, &result);
	zassert_equal(rc, ZDB_OK, "avg aggregate failed: %d", rc);
	zassert_equal(result.points, 3U, "expected 3 points, got %u", result.points);
	zassert_within(result.value, 20.0, 0.0001, "avg mismatch");

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_query_aggregate_min_max_sum)
{
	zdb_ts_t ts;
	const int64_t values[] = {5, 15, 25, 10};
	zdb_ts_agg_result_t result = {0};
	zdb_status_t rc = zdb_ts_open(&g_db, "t_mms", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_MIN, &result);
	zassert_equal(rc, ZDB_OK, "min aggregate failed: %d", rc);
	zassert_within(result.value, 5.0, 0.0001, "min mismatch");

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_MAX, &result);
	zassert_equal(rc, ZDB_OK, "max aggregate failed: %d", rc);
	zassert_within(result.value, 25.0, 0.0001, "max mismatch");

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_SUM, &result);
	zassert_equal(rc, ZDB_OK, "sum aggregate failed: %d", rc);
	zassert_within(result.value, 55.0, 0.0001, "sum mismatch");

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "count aggregate failed: %d", rc);
	zassert_equal(result.points, 4U, "expected 4 points, got %u", result.points);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_query_aggregate_window)
{
	zdb_ts_t ts;
	const int64_t values[] = {1, 2, 3, 4, 5};
	zdb_ts_agg_result_t result = {0};
	zdb_ts_window_t inner = { .from_ts_ms = 1001U, .to_ts_ms = 1003U };
	zdb_ts_window_t empty = { .from_ts_ms = 5000U, .to_ts_ms = 6000U };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_win", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&ts, inner, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "windowed count failed: %d", rc);
	zassert_equal(result.points, 3U, "expected 3 in-window points, got %u", result.points);

	rc = zdb_ts_query_aggregate(&ts, empty, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "empty window should be NOT_FOUND, got %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_cursor_iteration)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const int64_t values[] = {100, 101, 102};
	unsigned int count = 0U;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_cur", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor);
	zassert_equal(rc, ZDB_OK, "cursor open failed: %d", rc);

	while ((rc = zdb_cursor_next(&cursor, &record)) == ZDB_OK) {
		zassert_not_null(record.data, "record data NULL");
		zassert_true(record.len > 0U, "record length zero");
		count++;
		zassert_true(count <= ARRAY_SIZE(values), "cursor overran");
	}

	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "cursor should end with NOT_FOUND");
	zassert_equal(count, ARRAY_SIZE(values), "expected %zu records, got %u",
		      ARRAY_SIZE(values), count);

	rc = zdb_cursor_close(&cursor);
	zassert_equal(rc, ZDB_OK, "cursor close failed: %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_cursor_includes_unflushed)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const int64_t flushed[] = {1, 2};
	const int64_t unflushed[] = {3, 4};
	unsigned int count = 0U;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_ram", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, flushed, ARRAY_SIZE(flushed), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	ts_append_fixed(&ts, unflushed, ARRAY_SIZE(unflushed), 2000U);

	rc = zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor);
	zassert_equal(rc, ZDB_OK, "cursor open failed: %d", rc);

	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		count++;
	}

	zassert_equal(count, 4U, "expected flushed + RAM records, got %u", count);

	(void)zdb_cursor_close(&cursor);
	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_recover_noop_when_clean)
{
	zdb_ts_t ts;
	size_t truncated = 42U;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 7 };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_recok", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "append failed: %d", rc);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_recover_stream(&ts, &truncated);
	zassert_equal(rc, ZDB_OK, "recover failed: %d", rc);
	zassert_equal(truncated, 0U, "clean stream truncated %zu bytes", truncated);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_recover_truncates_garbage)
{
	zdb_ts_t ts;
	struct fs_file_t file;
	zdb_ts_agg_result_t result = {0};
	const int64_t values[] = {100, 200};
	const uint8_t garbage[13] = {
		0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33,
		0x44, 0x55, 0x66, 0x77, 0x88,
	};
	size_t truncated = 0U;
	int fs_rc;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_rec", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	/* Simulate a torn write: raw garbage appended to the stream file. */
	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/" CONFIG_ZDB_TS_DIRNAME "/t_rec.zts",
			FS_O_RDWR | FS_O_APPEND);
	zassert_equal(fs_rc, 0, "opening stream file failed: %d", fs_rc);
	fs_rc = fs_write(&file, garbage, sizeof(garbage));
	zassert_equal(fs_rc, (int)sizeof(garbage), "garbage write failed: %d", fs_rc);
	fs_rc = fs_close(&file);
	zassert_equal(fs_rc, 0, "closing stream file failed: %d", fs_rc);

	rc = zdb_ts_recover_stream(&ts, &truncated);
	zassert_equal(rc, ZDB_OK, "recover failed: %d", rc);
	zassert_equal(truncated, sizeof(garbage),
		      "expected %zu truncated bytes, got %zu", sizeof(garbage), truncated);

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "count after recovery failed: %d", rc);
	zassert_equal(result.points, 2U, "expected 2 surviving records, got %u",
		      result.points);

	zdb_ts_close(&ts);
}

#if defined(CONFIG_ZDB_STATS)
ZTEST(ts_suite, test_ts_stats_track_recover_runs)
{
	zdb_ts_t ts;
	zdb_ts_stats_t before;
	zdb_ts_stats_t after;
	size_t truncated = 0U;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_stats", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	zdb_ts_stats_get(&g_db, &before);
	rc = zdb_ts_recover_stream(&ts, &truncated);
	zassert_equal(rc, ZDB_OK, "recover failed: %d", rc);
	zdb_ts_stats_get(&g_db, &after);

	zassert_equal(after.recover_runs, before.recover_runs + 1U,
		      "recover_runs did not increment");

	zdb_ts_stats_reset(&g_db);
	zdb_ts_stats_get(&g_db, &after);
	zassert_equal(after.recover_runs, 0U, "stats reset did not clear counters");

	zdb_ts_close(&ts);
}
#endif /* CONFIG_ZDB_STATS */

#if defined(CONFIG_ZDB_FLATBUFFERS)
ZTEST(ts_suite, test_ts_sample_export_flatbuffer)
{
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 42 };
	uint8_t buffer[256];
	size_t out_len = 0U;
	zdb_status_t rc;

	rc = zdb_ts_sample_i64_export_flatbuffer(&sample, buffer, sizeof(buffer), &out_len);
	zassert_equal(rc, ZDB_OK, "flatbuffer export failed: %d", rc);
	zassert_true(out_len > 0U, "export produced empty output");
}
#endif /* CONFIG_ZDB_FLATBUFFERS */
