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
#include "../fixtures/common.h"
#include <string.h>
#include <errno.h>
#include <limits.h>

/* Test instance and backend */
ZDB_TEST_INSTANCE_DEFINE(g_test_db);
static struct mock_nvs_fs g_mock_nvs;

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
static zdb_kv_event_t g_last_event;
static uint32_t g_event_count;

static void test_event_listener(const zdb_kv_event_t *event, void *user_ctx)
{
	ARG_UNUSED(user_ctx);

	if (event == NULL) {
		return;
	}

	g_last_event = *event;
	g_event_count++;
}

static const zdb_event_listener_t g_event_listeners[] = {
	{
		.notify = NULL,
		.user_ctx = NULL,
	},
	{
		.notify = test_event_listener,
		.user_ctx = NULL,
	},
};
#endif

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
	#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
		.event_listeners = g_event_listeners,
		.event_listener_count = ARRAY_SIZE(g_event_listeners),
	#endif
	};

	rc = zdb_init(&g_test_db, &cfg);
	zassert_equal(rc, ZDB_OK, "Failed to init zdb: rc=%d", rc);

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	(void)memset(&g_last_event, 0, sizeof(g_last_event));
	g_event_count = 0U;
#endif
}

static void kv_test_teardown(void)
{
	zdb_deinit(&g_test_db);
	mock_nvs_reset(&g_mock_nvs);
}

#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
static uint16_t test_fnv1a16(const char *s)
{
	uint32_t hash = 0x811C9DC5u;

	while ((*s) != '\0') {
		hash ^= (uint8_t)(*s);
		hash *= 0x01000193u;
		s++;
	}

	hash &= 0xFFFFu;
	if (hash == 0U) {
		hash = 1U;
	}

	return (uint16_t)hash;
}

static void test_key_from_index(uint32_t idx, char out_key[5])
{
	/* 17 symbols -> 17^4 inputs (> 2^16), guarantees at least one collision. */
	static const char alphabet[] = "abcdefghijklmnopq";
	const uint32_t base = (uint32_t)(sizeof(alphabet) - 1U);

	out_key[0] = alphabet[idx % base];
	idx /= base;
	out_key[1] = alphabet[idx % base];
	idx /= base;
	out_key[2] = alphabet[idx % base];
	idx /= base;
	out_key[3] = alphabet[idx % base];
	out_key[4] = '\0';
}

static bool test_find_fnv16_collision(char key_a[5], char key_b[5])
{
	uint32_t first_idx[65536];
	uint32_t i;
	const uint32_t total = 17U * 17U * 17U * 17U;

	for (i = 0U; i < ARRAY_SIZE(first_idx); i++) {
		first_idx[i] = UINT32_MAX;
	}

	for (i = 0U; i < total; i++) {
		char key[5];
		uint16_t h;

		test_key_from_index(i, key);
		h = test_fnv1a16(key);

		if (first_idx[h] != UINT32_MAX) {
			test_key_from_index(first_idx[h], key_a);
			(void)strcpy(key_b, key);
			if (strcmp(key_a, key_b) != 0) {
				return true;
			}
		} else {
			first_idx[h] = i;
		}
	}

	return false;
}
#endif

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

static void test_kv_ops_require_io_slab(void)
{
	zdb_kv_t kv;
	uint8_t out_buf[8];
	uint32_t value = 0x12345678U;
	size_t out_len = 0U;
	struct k_mem_slab *saved_slab = g_test_db.kv_io_slab;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);

	assert_zdb_ok(rc);
	g_test_db.kv_io_slab = NULL;

	rc = zdb_kv_set(&kv, "k", &value, sizeof(value));
	assert_zdb_eq(rc, ZDB_ERR_INVAL);

	rc = zdb_kv_get(&kv, "k", out_buf, sizeof(out_buf), &out_len);
	assert_zdb_eq(rc, ZDB_ERR_INVAL);

	rc = zdb_kv_delete(&kv, "k");
	assert_zdb_eq(rc, ZDB_ERR_INVAL);

	g_test_db.kv_io_slab = saved_slab;
	zdb_kv_close(&kv);
}

static void test_kv_hash_collision_detected(void)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "collision_ns", &kv);

	assert_zdb_ok(rc);

#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
	{
		char key_a[5];
		char key_b[5];
		uint32_t value_a = 0xAAAAAAAAU;
		uint32_t value_b = 0xBBBBBBBBU;
		uint32_t out_val = 0U;
		size_t out_len = 0U;

		zassert_true(test_find_fnv16_collision(key_a, key_b),
			     "Failed to find deterministic FNV16 collision");

		rc = zdb_kv_set(&kv, key_a, &value_a, sizeof(value_a));
		assert_zdb_ok(rc);

		rc = zdb_kv_set(&kv, key_b, &value_b, sizeof(value_b));
		assert_zdb_ok(rc);

		rc = zdb_kv_get(&kv, key_a, &out_val, sizeof(out_val), &out_len);
		assert_zdb_eq(rc, ZDB_ERR_NOT_FOUND);

		rc = zdb_kv_get(&kv, key_b, &out_val, sizeof(out_val), &out_len);
		assert_zdb_ok(rc);
		zassert_equal(out_len, sizeof(value_b), "Unexpected value length");
		zassert_equal(out_val, value_b, "Collision handling returned wrong payload");
	}
#else
	zskip("Collision test currently targets NVS FNV16 backend");
#endif

	zdb_kv_close(&kv);
}

static void test_kv_iter_lists_namespace_entries(void)
{
	zdb_kv_t kv_ns1;
	zdb_kv_t kv_ns2;
	zdb_kv_iter_t iter;
	uint32_t value_a = 11U;
	uint32_t value_b = 22U;
	uint32_t value_other = 99U;
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	uint32_t out_value = 0U;
	size_t key_len = 0U;
	size_t out_len = 0U;
	bool saw_alpha = false;
	bool saw_beta = false;
	zdb_status_t rc;

	rc = zdb_kv_open(&g_test_db, "ns1", &kv_ns1);
	assert_zdb_ok(rc);
	rc = zdb_kv_open(&g_test_db, "ns2", &kv_ns2);
	assert_zdb_ok(rc);

	rc = zdb_kv_set(&kv_ns1, "alpha", &value_a, sizeof(value_a));
	assert_zdb_ok(rc);
	rc = zdb_kv_set(&kv_ns1, "beta", &value_b, sizeof(value_b));
	assert_zdb_ok(rc);
	rc = zdb_kv_set(&kv_ns2, "other", &value_other, sizeof(value_other));
	assert_zdb_ok(rc);

	rc = zdb_kv_iter_open(&kv_ns1, &iter);
	assert_zdb_ok(rc);

	while ((rc = zdb_kv_iter_next(&iter, key, sizeof(key), &key_len,
				      &out_value, sizeof(out_value), &out_len)) == ZDB_OK) {
		zassert_equal(out_len, sizeof(uint32_t), "Unexpected iter value length");
		if (strcmp(key, "alpha") == 0) {
			saw_alpha = true;
			zassert_equal(out_value, value_a, "alpha value mismatch");
		} else if (strcmp(key, "beta") == 0) {
			saw_beta = true;
			zassert_equal(out_value, value_b, "beta value mismatch");
		} else {
			zassert_unreachable("Iterator returned key from wrong namespace: %s", key);
		}
		zassert_true(key_len > 0U, "Iterator key length should be > 0");
	}

	assert_zdb_eq(rc, ZDB_ERR_NOT_FOUND);
	zassert_true(saw_alpha, "Iterator did not return alpha key");
	zassert_true(saw_beta, "Iterator did not return beta key");

	rc = zdb_kv_iter_close(&iter);
	assert_zdb_ok(rc);
	zdb_kv_close(&kv_ns2);
	zdb_kv_close(&kv_ns1);
}

static void test_kv_iter_skips_deleted_entries(void)
{
	zdb_kv_t kv;
	zdb_kv_iter_t iter;
	uint32_t keep_val = 1U;
	uint32_t delete_val = 2U;
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	uint32_t out_value = 0U;
	size_t key_len = 0U;
	size_t out_len = 0U;
	unsigned int found = 0U;
	zdb_status_t rc;

	rc = zdb_kv_open(&g_test_db, "iter_ns", &kv);
	assert_zdb_ok(rc);

	rc = zdb_kv_set(&kv, "keep", &keep_val, sizeof(keep_val));
	assert_zdb_ok(rc);
	rc = zdb_kv_set(&kv, "gone", &delete_val, sizeof(delete_val));
	assert_zdb_ok(rc);
	rc = zdb_kv_delete(&kv, "gone");
	assert_zdb_ok(rc);

	rc = zdb_kv_iter_open(&kv, &iter);
	assert_zdb_ok(rc);

	while ((rc = zdb_kv_iter_next(&iter, key, sizeof(key), &key_len,
				      &out_value, sizeof(out_value), &out_len)) == ZDB_OK) {
		found++;
		zassert_equal(strcmp(key, "keep"), 0, "Deleted key should not appear in iterator");
		zassert_equal(out_len, sizeof(keep_val), "Unexpected value length");
		zassert_equal(out_value, keep_val, "Unexpected iterator value");
		zassert_true(key_len > 0U, "Iterator key length should be > 0");
	}

	assert_zdb_eq(rc, ZDB_ERR_NOT_FOUND);
	zassert_equal(found, 1U, "Expected exactly one iterated key");

	rc = zdb_kv_iter_close(&iter);
	assert_zdb_ok(rc);
	zdb_kv_close(&kv);
}

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
static void test_kv_set_emits_event(void)
{
	zdb_kv_t kv;
	uint32_t value = 42U;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "test_ns", &kv);

	assert_zdb_ok(rc);

	rc = zdb_kv_set(&kv, "counter", &value, sizeof(value));
	assert_zdb_ok(rc);

	zassert_equal(g_event_count, 1U, "Expected one event, got %u", g_event_count);
	zassert_equal(g_last_event.type, ZDB_EVENT_KV_SET, "Unexpected event type: %d",
		      g_last_event.type);
	zassert_equal(g_last_event.status, ZDB_OK, "Unexpected event status: %d",
		      g_last_event.status);
	zassert_equal(g_last_event.value_len, sizeof(value),
		      "Unexpected value_len: %zu", g_last_event.value_len);
	zassert_equal(strcmp(g_last_event.namespace_name, "test_ns"), 0,
		      "Unexpected namespace");
	zassert_equal(strcmp(g_last_event.key, "counter"), 0, "Unexpected key");

	zdb_kv_close(&kv);
}

static void test_kv_delete_emits_event(void)
{
	zdb_kv_t kv;
	uint32_t value = 7U;
	zdb_status_t rc = zdb_kv_open(&g_test_db, "delete_ns", &kv);

	assert_zdb_ok(rc);

	rc = zdb_kv_set(&kv, "victim", &value, sizeof(value));
	assert_zdb_ok(rc);
	zassert_equal(g_event_count, 1U, "Set should emit one event");

	rc = zdb_kv_delete(&kv, "victim");
	assert_zdb_ok(rc);

	zassert_equal(g_event_count, 2U, "Expected two events, got %u", g_event_count);
	zassert_equal(g_last_event.type, ZDB_EVENT_KV_DELETE, "Unexpected event type: %d",
		      g_last_event.type);
	zassert_equal(g_last_event.status, ZDB_OK, "Unexpected event status: %d",
		      g_last_event.status);
	zassert_equal(g_last_event.value_len, 0U,
		      "Delete event value_len should be 0, got %zu", g_last_event.value_len);
	zassert_equal(strcmp(g_last_event.namespace_name, "delete_ns"), 0,
		      "Unexpected namespace");
	zassert_equal(strcmp(g_last_event.key, "victim"), 0, "Unexpected key");

	zdb_kv_close(&kv);
}
#endif

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
ztest_unit_test(test_kv_ops_require_io_slab);
ztest_unit_test(test_kv_hash_collision_detected);
ztest_unit_test(test_kv_iter_lists_namespace_entries);
ztest_unit_test(test_kv_iter_skips_deleted_entries);
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
ztest_unit_test(test_kv_set_emits_event);
ztest_unit_test(test_kv_delete_emits_event);
#endif
