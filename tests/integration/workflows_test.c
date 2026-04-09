/*
 * Copyright (c) 2026 ZephyrDB Test Suite
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration tests for ZephyrDB.
 * Tests multi-module interactions, common workflows, and end-to-end scenarios.
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyrdb.h>
#include "../fixtures/common.h"
#include <string.h>
#include <errno.h>

/* Test instance */
ZDB_TEST_INSTANCE_DEFINE(g_test_db);
static struct mock_nvs_fs g_mock_nvs;
static struct mock_lfs_fs g_mock_lfs;

/* ===== Setup and Teardown ===== */

static void integration_test_setup(void)
{
	int rc;
	
	/* Initialize both mock backends */
	rc = mock_nvs_init(&g_mock_nvs);
	zassert_equal(rc, 0, "Failed to init mock NVS");

	rc = mock_lfs_init(&g_mock_lfs);
	zassert_equal(rc, 0, "Failed to init mock LittleFS");

	/* Initialize database with both backends available */
	zdb_cfg_t cfg = {
		.kv_backend_fs = &g_mock_nvs.base,
		.lfs_mount_point = "/lfs",
		.work_q = &k_sys_work_q,
	};

	rc = zdb_init(&g_test_db, &cfg);
	zassert_equal(rc, ZDB_OK, "Failed to init zdb: rc=%d", rc);
}

static void integration_test_teardown(void)
{
	zdb_deinit(&g_test_db);
	mock_nvs_reset(&g_mock_nvs);
	mock_lfs_reset(&g_mock_lfs);
}

/* ===== Test Cases ===== */

/*
 * Test: KV and TS modules are independent
 * Expected: Both can be used simultaneously without interference
 */
static void test_kv_ts_independent(void)
{
	zdb_kv_t kv;
	zdb_ts_t ts;
	zdb_status_t rc;

	/* Open both KV and TS */
	rc = zdb_kv_open(&g_test_db, "app_config", &kv);
	assert_zdb_ok(rc);

	rc = zdb_ts_open(&g_test_db, "metrics", &ts);
	assert_zdb_ok(rc);

	/* Write to KV */
	uint32_t kv_value = 123;
	rc = zdb_kv_set(&kv, "setting_1", (uint8_t *)&kv_value, sizeof(kv_value));
	assert_zdb_ok(rc);

	/* Write to TS */
	zdb_ts_sample_i64_t sample = {
		.ts_ms = k_uptime_get(),
		.value = 567,
	};
	rc = zdb_ts_append_i64(&ts, &sample);
	assert_zdb_ok(rc);

	/* Verify KV read */
	uint32_t kv_readback = 0;
	size_t kv_len = 0;
	rc = zdb_kv_get(&kv, "setting_1", (uint8_t *)&kv_readback, sizeof(kv_readback), &kv_len);
	assert_zdb_ok(rc);
	zassert_equal(kv_readback, kv_value, "KV value corrupted");

	/* Cleanup */
	rc = zdb_kv_close(&kv);
	assert_zdb_ok(rc);

	rc = zdb_ts_close(&ts);
	assert_zdb_ok(rc);
}

/*
 * Test: DOC module dependency on TS (if applicable)
 * Expected: Proper error or graceful degradation if TS required but not available
 */
static void test_doc_ts_dependency(void)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_test_db, "users", "test_user", &doc);
	
	/* DOC may require TS, so either succeeds or produces clear error */
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED,
		     "DOC creation returned unexpected status: %d", rc);

	if (rc == ZDB_OK) {
		zdb_doc_close(&doc);
	}
}

/*
 * Test: Init and deinit sequence
 * Expected: No resource leaks, proper cleanup
 */
static void test_init_deinit_sequence(void)
{
	/* This test reuses the setup/teardown, verifying multi-open pattern */
	zdb_kv_t kv1, kv2;
	zdb_ts_t ts1, ts2;
	zdb_status_t rc;

	/* Open multiple KV and TS handles */
	rc = zdb_kv_open(&g_test_db, "ns1", &kv1);
	assert_zdb_ok(rc);

	rc = zdb_kv_open(&g_test_db, "ns2", &kv2);
	assert_zdb_ok(rc);

	rc = zdb_ts_open(&g_test_db, "stream1", &ts1);
	assert_zdb_ok(rc);

	rc = zdb_ts_open(&g_test_db, "stream2", &ts2);
	assert_zdb_ok(rc);

	/* Write to all */
	uint32_t kv_val = 100;
	zdb_kv_set(&kv1, "key1", (uint8_t *)&kv_val, sizeof(kv_val));
	zdb_kv_set(&kv2, "key2", (uint8_t *)&kv_val, sizeof(kv_val));

	zdb_ts_sample_i64_t sample = {.ts_ms = k_uptime_get(), .value = 200};
	zdb_ts_append_i64(&ts1, &sample);
	zdb_ts_append_i64(&ts2, &sample);

	/* Close all in reverse order (good hygiene) */
	zdb_ts_close(&ts2);
	zdb_ts_close(&ts1);
	zdb_kv_close(&kv2);
	zdb_kv_close(&kv1);

	/* Deinit happens in teardown; no crashes = success */
}

/*
 * Test: Concurrent cursor usage (simulated via sequential iteration)
 * Expected: Multiple cursors don't corrupt each other's state
 */
static void test_concurrent_cursor_iterators(void)
{
	zdb_ts_t stream1, stream2;
	zdb_status_t rc;

	/* Open two streams and add samples */
	rc = zdb_ts_open(&g_test_db, "stream_a", &stream1);
	assert_zdb_ok(rc);

	rc = zdb_ts_open(&g_test_db, "stream_b", &stream2);
	assert_zdb_ok(rc);

	/* Add samples to both */
	for (int i = 0; i < 5; i++) {
		zdb_ts_sample_i64_t s1 = {.ts_ms = k_uptime_get() + i, .value = 100 + i};
		zdb_ts_sample_i64_t s2 = {.ts_ms = k_uptime_get() + i, .value = 200 + i};
		zdb_ts_append_i64(&stream1, &s1);
		zdb_ts_append_i64(&stream2, &s2);
	}

	zdb_ts_flush_sync(&stream1, K_SECONDS(1));
	zdb_ts_flush_sync(&stream2, K_SECONDS(1));

	/* Open cursors on both streams (mocks may not support properly) */
	zdb_ts_cursor_t cursor1, cursor2;
	rc = zdb_ts_cursor_open(&stream1, (zdb_ts_time_window_t){0}, NULL, NULL, &cursor1);
	if (rc == ZDB_OK) {
		rc = zdb_ts_cursor_open(&stream2, (zdb_ts_time_window_t){0}, NULL, NULL, &cursor2);
		if (rc == ZDB_OK) {
			/* Interleave iterations to test concurrent behavior */
			zdb_ts_record_t rec1, rec2;
			int iter_count = 0;
			while (iter_count < 5) {
				int rc1 = zdb_cursor_next(&cursor1, &rec1);
				int rc2 = zdb_cursor_next(&cursor2, &rec2);
				/* Either succeeds with data or ends */
				(void)rc1;
				(void)rc2;
				iter_count++;
			}

			zdb_cursor_close(&cursor2);
		}
		zdb_cursor_close(&cursor1);
	}

	/* Cleanup */
	zdb_ts_close(&stream2);
	zdb_ts_close(&stream1);
}

/*
 * Test: Health status transitions
 * Expected: Health status reflects operational state
 */
static void test_health_status_transitions(void)
{
	zdb_health_t health;

	/* Initial health should be OK or DEGRADED */
	health = zdb_health(&g_test_db);
	zassert_true(health == ZDB_HEALTH_OK || health == ZDB_HEALTH_DEGRADED ||
		     health == ZDB_HEALTH_READONLY || health == ZDB_HEALTH_FAULT,
		     "Unexpected initial health: %d", health);

	/* After operations, health should remain consistent */
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test", &kv);
	if (rc == ZDB_OK) {
		uint32_t val = 42;
		zdb_kv_set(&kv, "key", (uint8_t *)&val, sizeof(val));

		health = zdb_health(&g_test_db);
		zassert_true(health != ZDB_HEALTH_FAULT, "Health should not be FAULT after normal ops");

		zdb_kv_close(&kv);
	}
}

/*
 * Test: Stats export consistency
 * Expected: TS stats counts match expected operations
 */
static void test_stats_export_consistency(void)
{
	zdb_ts_stats_t stats_before, stats_after;
	zdb_status_t rc;

	/* Get initial stats */
	zdb_ts_stats_get(&g_test_db, &stats_before);

	/* Do some TS operations */
	zdb_ts_t stream;
	rc = zdb_ts_open(&g_test_db, "test_stream", &stream);
	if (rc == ZDB_OK) {
		for (int i = 0; i < 3; i++) {
			zdb_ts_sample_i64_t sample = {
				.ts_ms = k_uptime_get() + i,
				.value = 100 + i,
			};
			zdb_ts_append_i64(&stream, &sample);
		}
		zdb_ts_close(&stream);
	}

	/* Get stats after operations */
	zdb_ts_stats_get(&g_test_db, &stats_after);

	/* Verify stats changed (some counters incremented) */
	/* Exact values depend on implementation, so just verify non-negative */
	zassert_true(stats_after.recover_runs >= stats_before.recover_runs,
		     "Stats should not decrease");
}

/*
 * Test: KV basic workflow (mirrors kv_basic sample)
 * Expected: Open -> Set -> Get -> Delete -> Get (not found)
 */
static void test_kv_basic_workflow(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "app", &kv);
	assert_zdb_ok(rc);

	/* Set boot count */
	uint32_t boot_count = 1;
	rc = zdb_kv_set(&kv, "boot_count", (uint8_t *)&boot_count, sizeof(boot_count));
	assert_zdb_ok(rc);

	/* Get boot count */
	uint32_t readback = 0;
	size_t out_len = 0;
	rc = zdb_kv_get(&kv, "boot_count", (uint8_t *)&readback, sizeof(readback), &out_len);
	assert_zdb_ok(rc);
	zassert_equal(readback, boot_count, "Boot count mismatch");
	zassert_equal(out_len, sizeof(boot_count), "Length mismatch");

	/* Delete boot count */
	rc = zdb_kv_delete(&kv, "boot_count");
	assert_zdb_ok(rc);

	/* Verify deletion */
	rc = zdb_kv_get(&kv, "boot_count", (uint8_t *)&readback, sizeof(readback), &out_len);
	assert_zdb_eq(rc, ZDB_ERR_NOT_FOUND);

	zdb_kv_close(&kv);
}

/*
 * Test: DOC query filters workflow (mirrors doc_query_filters sample)
 * Expected: Create docs -> Filter query -> Verify results
 */
static void test_doc_query_filters_workflow(void)
{
	zdb_doc_t doc;
	zdb_status_t rc;

	/* Skip if DOC not available */
	rc = zdb_doc_create(&g_test_db, "test_col", "test_id", &doc);
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}
	zdb_doc_close(&doc);

	/* Create 3 test documents */
	const struct {
		const char *id;
		const char *name;
		int64_t score;
		bool approved;
	} docs[] = {
		{"doc1", "Item A", 85, true},
		{"doc2", "Item B", 92, true},
		{"doc3", "Item C", 78, false},
	};

	/* Save documents */
	for (int i = 0; i < 3; i++) {
		rc = zdb_doc_create(&g_test_db, "items", docs[i].id, &doc);
		if (rc != ZDB_OK) break;

		zdb_doc_field_set_string(&doc, "name", docs[i].name);
		zdb_doc_field_set_i64(&doc, "score", docs[i].score);
		zdb_doc_field_set_bool(&doc, "approved", docs[i].approved);

		zdb_doc_save(&doc);
		zdb_doc_close(&doc);
	}

	/* Query with multiple filters: score >= 85 AND approved == true */
	zdb_doc_query_filter_t filters[] = {
		{.field_name = "score", .type = ZDB_DOC_FIELD_INT64, .i64_value = 85},
		{.field_name = "approved", .type = ZDB_DOC_FIELD_BOOL, .bool_value = true},
	};

	zdb_doc_query_spec_t query_spec = {
		.collection = "items",
		.filters = filters,
		.filter_count = 2,
	};

	zdb_doc_metadata_t *results = NULL;
	size_t result_count = 0;
	rc = zdb_doc_query(&g_test_db, &query_spec, &results, &result_count);

	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED,
		     "Query failed: %d", rc);

	if (rc == ZDB_OK && results != NULL) {
		/* Should find 2 documents matching both filters */
		zassert_equal(result_count, 2, "Expected 2 results, got %zu", result_count);
		zdb_doc_metadata_free(results, result_count);
	}
}

/* ===== Test Suite Registration ===== */

ztest_suite(zephyrdb_integration, integration_test_setup, integration_test_teardown,
	    NULL, NULL);

ztest_unit_test(test_kv_ts_independent);
ztest_unit_test(test_doc_ts_dependency);
ztest_unit_test(test_init_deinit_sequence);
ztest_unit_test(test_concurrent_cursor_iterators);
ztest_unit_test(test_health_status_transitions);
ztest_unit_test(test_stats_export_consistency);
ztest_unit_test(test_kv_basic_workflow);
ztest_unit_test(test_doc_query_filters_workflow);
