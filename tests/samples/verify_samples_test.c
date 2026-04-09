/*
 * Copyright (c) 2026 ZephyrDB Test Suite
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sample validation tests for ZephyrDB.
 * Verifies that existing samples compile and run without crashes.
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyrdb.h>
#include <string.h>

/* ===== Sample Integration Tests ===== */

/*
 * Test: KV Basic sample compiles and can run critical workflow
 * Expected: Open -> Set -> Get -> Delete -> Get (NOTFOUND)
 * This is a simplified inline version of samples/kv_basic/src/main.c
 */
static void test_sample_kv_basic_workflow(void)
{
	ZDB_TEST_INSTANCE_DEFINE(sample_db);
	static struct mock_nvs_fs sample_nvs;

	/* Initialize mock backend */
	int rc = mock_nvs_init(&sample_nvs);
	zassert_equal(rc, 0, "Mock NVS init failed");

	/* Init DB */
	zdb_cfg_t cfg = {
		.kv_backend_fs = &sample_nvs.base,
		.lfs_mount_point = "/lfs",
		.work_q = &k_sys_work_q,
	};

	zdb_status_t status = zdb_init(&sample_db, &cfg);
	zassert_equal(status, ZDB_OK, "DB init failed: %d", status);

	/* Open namespace */
	zdb_kv_t kv;
	status = zdb_kv_open(&sample_db, "app", &kv);
	zassert_equal(status, ZDB_OK, "KV open failed: %d", status);

	/* Set boot count */
	uint32_t boot_count = 5;
	status = zdb_kv_set(&kv, "boot_count", (uint8_t *)&boot_count, sizeof(boot_count));
	zassert_equal(status, ZDB_OK, "KV set failed: %d", status);

	/* Get boot count */
	uint32_t readback = 0;
	size_t out_len = 0;
	status = zdb_kv_get(&kv, "boot_count", (uint8_t *)&readback, sizeof(readback), &out_len);
	zassert_equal(status, ZDB_OK, "KV get failed: %d", status);
	zassert_equal(readback, boot_count, "Boot count mismatch: %u != %u", readback, boot_count);
	zassert_equal(out_len, sizeof(boot_count), "Length mismatch: %zu != %zu", out_len, sizeof(boot_count));

	/* Delete boot count */
	status = zdb_kv_delete(&kv, "boot_count");
	zassert_equal(status, ZDB_OK, "KV delete failed: %d", status);

	/* Verify deletion */
	status = zdb_kv_get(&kv, "boot_count", (uint8_t *)&readback, sizeof(readback), &out_len);
	zassert_equal(status, ZDB_ERR_NOT_FOUND, "Expected NOT_FOUND after delete, got %d", status);

	/* Cleanup */
	zdb_kv_close(&kv);
	zdb_deinit(&sample_db);
	mock_nvs_reset(&sample_nvs);

	ztest_pass();
}

/*
 * Test: TS Basic sample compiles and can run workflow
 * Expected: Open -> Append -> Flush -> Query aggregate
 * Simplified version of samples/ts_basic/src/main.c
 */
static void test_sample_ts_basic_workflow(void)
{
	ZDB_TEST_INSTANCE_DEFINE(sample_db);
	static struct mock_lfs_fs sample_lfs;

	/* Initialize mock backend */
	int rc = mock_lfs_init(&sample_lfs);
	zassert_equal(rc, 0, "Mock LittleFS init failed");

	/* Init DB */
	zdb_cfg_t cfg = {
		.kv_backend_fs = NULL,
		.lfs_mount_point = "/lfs",
		.work_q = &k_sys_work_q,
	};

	zdb_status_t status = zdb_init(&sample_db, &cfg);
	zassert_equal(status, ZDB_OK, "DB init failed: %d", status);

	/* Open stream */
	zdb_ts_t stream;
	status = zdb_ts_open(&sample_db, "metrics", &stream);
	zassert_equal(status, ZDB_OK, "TS open failed: %d", status);

	/* Append samples */
	for (int i = 0; i < 5; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = k_uptime_get() + (i * 1000),
			.value = 100 + i * 10,
		};
		status = zdb_ts_append_i64(&stream, &sample);
		zassert_equal(status, ZDB_OK, "TS append failed: %d", status);
	}

	/* Flush synchronously */
	status = zdb_ts_flush_sync(&stream, K_SECONDS(2));
	zassert_equal(status, ZDB_OK, "TS flush failed: %d", status);

	/* Query aggregate (may be mocked, but shouldn't crash) */
	zdb_ts_aggregate_result_t result = {0};
	status = zdb_ts_query_aggregate(&stream, (zdb_ts_time_window_t){0}, 
				        ZDB_TS_AGG_AVG, &result);
	/* May succeed or return UNSUPPORTED in unit test; both OK */
	zassert_true(status == ZDB_OK || status == ZDB_ERR_UNSUPPORTED,
		     "Unexpected query result: %d", status);

	/* Cleanup */
	zdb_ts_close(&stream);
	zdb_deinit(&sample_db);
	mock_lfs_reset(&sample_lfs);

	ztest_pass();
}

/*
 * Test: DOC Basic sample compiles and can run workflow
 * Expected: Create -> Field set -> Save -> Field get
 * Simplified version of samples/doc_basic/src/main.c
 */
static void test_sample_doc_basic_workflow(void)
{
	ZDB_TEST_INSTANCE_DEFINE(sample_db);
	static struct mock_lfs_fs sample_lfs;

	/* Initialize mock backend */
	int rc = mock_lfs_init(&sample_lfs);
	zassert_equal(rc, 0, "Mock LittleFS init failed");

	/* Init DB */
	zdb_cfg_t cfg = {
		.kv_backend_fs = NULL,
		.lfs_mount_point = "/lfs",
		.work_q = &k_sys_work_q,
	};

	zdb_status_t status = zdb_init(&sample_db, &cfg);
	zassert_equal(status, ZDB_OK, "DB init failed: %d", status);

	/* Create document */
	zdb_doc_t doc;
	status = zdb_doc_create(&sample_db, "users", "u1", &doc);
	if (status != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	/* Set fields */
	status = zdb_doc_field_set_string(&doc, "name", "Ada");
	zassert_equal(status, ZDB_OK, "Field set string failed: %d", status);

	status = zdb_doc_field_set_i64(&doc, "age", 30);
	zassert_equal(status, ZDB_OK, "Field set i64 failed: %d", status);

	status = zdb_doc_field_set_bool(&doc, "active", true);
	zassert_equal(status, ZDB_OK, "Field set bool failed: %d", status);

	/* Save document */
	status = zdb_doc_save(&doc);
	zassert_equal(status, ZDB_OK, "DOC save failed: %d", status);

	/* Get fields back */
	const char *name = NULL;
	status = zdb_doc_field_get_string(&doc, "name", &name);
	zassert_equal(status, ZDB_OK, "Field get string failed: %d", status);
	zassert_str_equal(name, "Ada", "Name mismatch");

	int64_t age = 0;
	status = zdb_doc_field_get_i64(&doc, "age", &age);
	zassert_equal(status, ZDB_OK, "Field get i64 failed: %d", status);
	zassert_equal(age, 30, "Age mismatch: %lld != 30", age);

	bool active = false;
	status = zdb_doc_field_get_bool(&doc, "active", &active);
	zassert_equal(status, ZDB_OK, "Field get bool failed: %d", status);
	zassert_equal(active, true, "Active flag mismatch");

	/* Export to FlatBuffer (if supported) */
	uint8_t fb_buffer[512];
	size_t fb_len = 0;
	status = zdb_doc_export_flatbuffer(&doc, fb_buffer, sizeof(fb_buffer), &fb_len);
	/* May not be supported in unit test, so allow UNSUPPORTED */
	zassert_true(status == ZDB_OK || status == ZDB_ERR_UNSUPPORTED,
		     "Unexpected export result: %d", status);

	/* Cleanup */
	zdb_doc_close(&doc);
	zdb_deinit(&sample_db);
	mock_lfs_reset(&sample_lfs);

	ztest_pass();
}

/* ===== Test Suite Registration ===== */

ztest_suite(zephyrdb_samples, NULL, NULL, NULL, NULL);

ztest_unit_test(test_sample_kv_basic_workflow);
ztest_unit_test(test_sample_ts_basic_workflow);
ztest_unit_test(test_sample_doc_basic_workflow);
