/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Aggregate-query tests against the real LittleFS backend, using the fstab
 * automounted /lfs filesystem provided by boards/native_sim.overlay.
 *
 * This suite builds with CONFIG_ZDB_TS_MAX_AGG_POINTS=8 so the scan cap is
 * reachable: COUNT must ignore it, and the value-bearing aggregates must
 * report when they hit it.
 *
 * Each test runs on a fresh zdb instance (the TS context pins one active
 * stream per instance until deinit) and uses its own stream name.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <string.h>

#include "zephyrdb.h"

#define AGG_CAP CONFIG_ZDB_TS_MAX_AGG_POINTS

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void agg_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void agg_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(agg_suite, NULL, NULL, agg_before, agg_after, NULL);

/* Append @p count samples with value == index, timestamps starting at 1000. */
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

/*
 * COUNT used to stop at CONFIG_ZDB_TS_MAX_AGG_POINTS and report the capped
 * number as if it were the whole stream.
 */
ZTEST(agg_suite, test_agg_count_exceeds_cap)
{
	zdb_ts_t ts;
	zdb_ts_agg_result_t result = {0};
	const size_t total = (AGG_CAP * 2U) + 3U;

	zassert_equal(zdb_ts_open(&g_db, "a_count", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result),
		      ZDB_OK, "count failed");
	zassert_equal(result.points, total, "count truncated at the cap: %u", result.points);
	zassert_equal((size_t)result.value, total, "count value mismatch");
	zassert_false(result.truncated, "COUNT must never report truncation");

	(void)zdb_ts_close(&ts);
}

/* The same, through the scanning path: a window disables the fast count. */
ZTEST(agg_suite, test_agg_count_exceeds_cap_windowed)
{
	zdb_ts_t ts;
	zdb_ts_agg_result_t result = {0};
	const size_t total = (AGG_CAP * 2U) + 3U;
	zdb_ts_window_t window = {.from_ts_ms = 1001U, .to_ts_ms = 1000U + total - 1U};

	zassert_equal(zdb_ts_open(&g_db, "a_count_win", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_query_aggregate(&ts, window, ZDB_TS_AGG_COUNT, &result), ZDB_OK,
		      "windowed count failed");
	/* The window excludes only the first sample. */
	zassert_equal(result.points, total - 1U, "windowed count wrong: %u", result.points);
	zassert_false(result.truncated, "COUNT must never report truncation");

	(void)zdb_ts_close(&ts);
}

/* Unflushed samples are part of the stream and must be counted. */
ZTEST(agg_suite, test_agg_count_includes_unflushed)
{
	zdb_ts_t ts;
	zdb_ts_agg_result_t result = {0};

	zassert_equal(zdb_ts_open(&g_db, "a_count_ram", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, 3U);

	zassert_equal(zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result),
		      ZDB_OK, "count failed");
	zassert_equal(result.points, 3U, "unflushed samples not counted: %u", result.points);

	(void)zdb_ts_close(&ts);
}

/* An empty result is a real answer for COUNT, not an error. */
ZTEST(agg_suite, test_agg_count_empty_window_is_zero)
{
	zdb_ts_t ts;
	zdb_ts_agg_result_t result = {.points = 99U};
	zdb_ts_window_t empty = {.from_ts_ms = 500000U, .to_ts_ms = 500100U};

	zassert_equal(zdb_ts_open(&g_db, "a_empty", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, 4U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_query_aggregate(&ts, empty, ZDB_TS_AGG_COUNT, &result), ZDB_OK,
		      "empty-window count should succeed");
	zassert_equal(result.points, 0U, "expected zero, got %u", result.points);
	zassert_equal((int)result.value, 0, "expected zero value");

	(void)zdb_ts_close(&ts);
}

/* The value-bearing aggregates still have nothing to report when empty. */
ZTEST(agg_suite, test_agg_sum_empty_window_not_found)
{
	zdb_ts_t ts;
	zdb_ts_agg_result_t result = {0};
	zdb_ts_window_t empty = {.from_ts_ms = 500000U, .to_ts_ms = 500100U};

	zassert_equal(zdb_ts_open(&g_db, "a_empty_sum", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, 4U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_query_aggregate(&ts, empty, ZDB_TS_AGG_SUM, &result),
		      ZDB_ERR_NOT_FOUND, "expected NOT_FOUND for empty SUM");

	(void)zdb_ts_close(&ts);
}

/* Capped aggregates report the cap instead of passing off a partial answer. */
ZTEST(agg_suite, test_agg_sum_reports_truncation)
{
	zdb_ts_t ts;
	zdb_ts_agg_result_t result = {0};
	const size_t total = AGG_CAP * 2U;
	double capped_sum = 0.0;

	zassert_equal(zdb_ts_open(&g_db, "a_trunc", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, total);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_SUM, &result),
		      ZDB_OK, "sum failed");
	zassert_equal(result.points, (uint32_t)AGG_CAP, "expected the cap, got %u", result.points);
	zassert_true(result.truncated, "truncation not reported");

	/* Values are 0..AGG_CAP-1, so the partial sum is well defined. */
	for (size_t i = 0U; i < (size_t)AGG_CAP; i++) {
		capped_sum += (double)i;
	}
	zassert_within(result.value, capped_sum, 0.001, "partial sum wrong");

	(void)zdb_ts_close(&ts);
}

/* A result that fits under the cap is complete, not truncated. */
ZTEST(agg_suite, test_agg_sum_exactly_at_cap_not_truncated)
{
	zdb_ts_t ts;
	zdb_ts_agg_result_t result = {0};

	zassert_equal(zdb_ts_open(&g_db, "a_exact", &ts), ZDB_OK, "open failed");
	append_ramp(&ts, (size_t)AGG_CAP);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_SUM, &result),
		      ZDB_OK, "sum failed");
	zassert_equal(result.points, (uint32_t)AGG_CAP, "expected the cap, got %u", result.points);
	zassert_false(result.truncated, "exhausted stream reported as truncated");

	(void)zdb_ts_close(&ts);
}
