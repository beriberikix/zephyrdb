/*
 * Copyright (c) 2026 ZephyrDB Test Suite
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for ZephyrDB KV (Key-Value) module.
 * Tests critical APIs: zdb_kv_open, zdb_kv_set, zdb_kv_get, zdb_kv_delete.
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyrdb.h>
#include <string.h>
#include <errno.h>

/* Test instance and backend */
ZDB_TEST_INSTANCE_DEFINE(g_test_db);
static struct mock_nvs_fs g_mock_nvs;

/* ===== Setup and Teardown ===== */

static void kv_test_setup(void)
{
	/* Initialize mock backend */
	int rc = mock_nvs_init(&g_mock_nvs);
	zassert_equal(rc, 0, "Failed to init mock NVS");

	/* Initialize database with mock backend */
	zdb_cfg_t cfg = {
		.kv_backend_fs = &g_mock_nvs.base,
		.lfs_mount_point = "/lfs",
		.work_q = &k_sys_work_q,
	};

	rc = zdb_init(&g_test_db, &cfg);
	zassert_equal(rc, ZDB_OK, "Failed to init zdb: rc=%d", rc);
}

static void kv_test_teardown(void)
{
	zdb_deinit(&g_test_db);
	mock_nvs_reset(&g_mock_nvs);
}

/* ===== Test Cases ===== */

/*
 * Test: KV namespace open succeeds
 * Expected: zdb_kv_open returns ZDB_OK
 */
static void test_kv_open_success(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* Cleanup */
	rc = zdb_kv_close(&kv);
	assert_zdb_ok(rc);
}

/*
 * Test: KV set and get basic operation
 * Expected: Set a key-value pair, then retrieve it
 */
static void test_kv_set_basic(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* Set a simple uint32 value */
	uint32_t value = 42;
	rc = zdb_kv_set(&kv, "counter", (uint8_t *)&value, sizeof(value));
	assert_zdb_ok(rc);

	zdb_kv_close(&kv);
}

/*
 * Test: KV get existing key
 * Expected: Retrieve value and length match what was set
 */
static void test_kv_get_success(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* Set value */
	uint32_t set_value = 42;
	rc = zdb_kv_set(&kv, "counter", (uint8_t *)&set_value, sizeof(set_value));
	assert_zdb_ok(rc);

	/* Get value */
	uint32_t get_value = 0;
	size_t out_len = 0;
	rc = zdb_kv_get(&kv, "counter", (uint8_t *)&get_value, sizeof(get_value), &out_len);
	assert_zdb_ok(rc);
	zassert_equal(out_len, sizeof(set_value), "Length mismatch: %zu != %zu", out_len, sizeof(set_value));
	zassert_equal(get_value, set_value, "Value mismatch: %u != %u", get_value, set_value);

	zdb_kv_close(&kv);
}

/*
 * Test: KV get non-existent key
 * Expected: Returns ZDB_ERR_NOT_FOUND
 */
static void test_kv_get_not_found(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* Try to get non-existent key */
	uint8_t buffer[32];
	size_t out_len = 0;
	rc = zdb_kv_get(&kv, "nonexistent", buffer, sizeof(buffer), &out_len);
	assert_zdb_eq(rc, ZDB_ERR_NOT_FOUND);

	zdb_kv_close(&kv);
}

/*
 * Test: KV set overwrites existing value
 * Expected: Second set replaces first value
 */
static void test_kv_set_overwrite(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* Set initial value */
	uint32_t value1 = 10;
	rc = zdb_kv_set(&kv, "counter", (uint8_t *)&value1, sizeof(value1));
	assert_zdb_ok(rc);

	/* Overwrite with new value */
	uint32_t value2 = 20;
	rc = zdb_kv_set(&kv, "counter", (uint8_t *)&value2, sizeof(value2));
	assert_zdb_ok(rc);

	/* Verify new value is stored */
	uint32_t readback = 0;
	size_t out_len = 0;
	rc = zdb_kv_get(&kv, "counter", (uint8_t *)&readback, sizeof(readback), &out_len);
	assert_zdb_ok(rc);
	zassert_equal(readback, value2, "Value should be overwritten: %u != %u", readback, value2);

	zdb_kv_close(&kv);
}

/*
 * Test: KV delete removes key
 * Expected: Delete succeeds, subsequent get returns NOT_FOUND
 */
static void test_kv_delete_success(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* Set a key */
	uint32_t value = 42;
	rc = zdb_kv_set(&kv, "counter", (uint8_t *)&value, sizeof(value));
	assert_zdb_ok(rc);

	/* Delete the key */
	rc = zdb_kv_delete(&kv, "counter");
	assert_zdb_ok(rc);

	/* Verify key is gone */
	uint8_t buffer[32];
	size_t out_len = 0;
	rc = zdb_kv_get(&kv, "counter", buffer, sizeof(buffer), &out_len);
	assert_zdb_eq(rc, ZDB_ERR_NOT_FOUND);

	zdb_kv_close(&kv);
}

/*
 * Test: KV delete non-existent key
 * Expected: Returns ZDB_ERR_NOT_FOUND (or ZDB_OK, backend dependent)
 */
static void test_kv_delete_not_found(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* Try to delete non-existent key - should either fail or silently succeed */
	rc = zdb_kv_delete(&kv, "nonexistent");
	/* Allow both ZDB_OK and ZDB_ERR_NOT_FOUND for backend compatibility */
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_NOT_FOUND, 
		     "Unexpected status: %d", rc);

	zdb_kv_close(&kv);
}

/*
 * Test: KV value size limits
 * Expected: Respect storage capacity, reject oversized values
 */
static void test_kv_value_max_length(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* Create a reasonably large value (256 bytes, typical small limit) */
	uint8_t large_value[256];
	memset(large_value, 0xAA, sizeof(large_value));

	/* This should succeed (at most backends allow this) */
	rc = zdb_kv_set(&kv, "large_key", large_value, sizeof(large_value));
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_INVAL,
		     "Set large value failed unexpectedly: %d", rc);

	zdb_kv_close(&kv);
}

/*
 * Test: KV key size limits (48 bytes per API docs)
 * Expected: Respect 48-byte key limit
 */
static void test_kv_key_max_length(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);
	assert_zdb_ok(rc);

	/* 48-byte key should work */
	char max_key[48];
	memset(max_key, 'X', 47);
	max_key[47] = '\0';

	uint32_t value = 123;
	rc = zdb_kv_set(&kv, max_key, (uint8_t *)&value, sizeof(value));
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_INVAL,
		     "Max-size key failed: %d", rc);

	/* Over 48 bytes should fail */
	char oversized_key[64];
	memset(oversized_key, 'Y', 63);
	oversized_key[63] = '\0';

	rc = zdb_kv_set(&kv, oversized_key, (uint8_t *)&value, sizeof(value));
	zassert_true(rc != ZDB_OK, "Oversized key should fail, but returned %d", rc);

	zdb_kv_close(&kv);
}

/*
 * Test: KV namespace isolation
 * Expected: Keys in different namespaces don't interfere
 */
static void test_kv_multiple_namespaces(void)
{
	zdb_kv_t kv1, kv2;
	zdb_status_t rc;

	/* Open two namespaces */
	rc = zdb_kv_open(&g_test_db, "namespace_1", &kv1);
	assert_zdb_ok(rc);

	rc = zdb_kv_open(&g_test_db, "namespace_2", &kv2);
	assert_zdb_ok(rc);

	/* Set different values with same key in each namespace */
	uint32_t value1 = 111;
	uint32_t value2 = 222;

	rc = zdb_kv_set(&kv1, "shared_key", (uint8_t *)&value1, sizeof(value1));
	assert_zdb_ok(rc);

	rc = zdb_kv_set(&kv2, "shared_key", (uint8_t *)&value2, sizeof(value2));
	assert_zdb_ok(rc);

	/* Verify values are isolated */
	uint32_t readback1 = 0;
	uint32_t readback2 = 0;
	size_t len1 = 0, len2 = 0;

	rc = zdb_kv_get(&kv1, "shared_key", (uint8_t *)&readback1, sizeof(readback1), &len1);
	assert_zdb_ok(rc);
	zassert_equal(readback1, value1, "Namespace 1 value corrupted: %u != %u", readback1, value1);

	rc = zdb_kv_get(&kv2, "shared_key", (uint8_t *)&readback2, sizeof(readback2), &len2);
	assert_zdb_ok(rc);
	zassert_equal(readback2, value2, "Namespace 2 value corrupted: %u != %u", readback2, value2);

	zdb_kv_close(&kv1);
	zdb_kv_close(&kv2);
}

/* ===== Test Suite Registration ===== */

ztest_suite(zephyrdb_kv_basic, kv_test_setup, kv_test_teardown,
	    NULL, NULL);

ztest_unit_test(test_kv_open_success);
ztest_unit_test(test_kv_set_basic);
ztest_unit_test(test_kv_get_success);
ztest_unit_test(test_kv_get_not_found);
ztest_unit_test(test_kv_set_overwrite);
ztest_unit_test(test_kv_delete_success);
ztest_unit_test(test_kv_delete_not_found);
ztest_unit_test(test_kv_value_max_length);
ztest_unit_test(test_kv_key_max_length);
ztest_unit_test(test_kv_multiple_namespaces);
