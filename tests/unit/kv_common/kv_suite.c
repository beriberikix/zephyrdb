/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * KV unit tests against a real backend (ZMS or NVS, selected by the
 * enclosing test application's prj.conf) mounted on the native_sim
 * flash simulator's storage_partition.
 *
 * Shared by tests/unit/kv_zms and tests/unit/kv_nvs.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>

#include "zephyrdb.h"

#if defined(CONFIG_ZDB_KV_BACKEND_ZMS)
#include <zephyr/kvss/zms.h>
static struct zms_fs g_backend;
#elif defined(CONFIG_ZDB_KV_BACKEND_NVS)
#include <zephyr/kvss/nvs.h>
static struct nvs_fs g_backend;
#else
#error "kv_suite requires CONFIG_ZDB_KV_BACKEND_ZMS or CONFIG_ZDB_KV_BACKEND_NVS"
#endif

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = NULL,
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

#define KV_TEST_PARTITION storage_partition

static void kv_backend_mount(void)
{
	struct flash_pages_info info;
	int rc;

	g_backend.flash_device = PARTITION_DEVICE(KV_TEST_PARTITION);
	zassert_true(device_is_ready(g_backend.flash_device),
		     "storage device not ready");

	g_backend.offset = PARTITION_OFFSET(KV_TEST_PARTITION);
	rc = flash_get_page_info_by_offs(g_backend.flash_device, g_backend.offset, &info);
	zassert_equal(rc, 0, "flash page info failed: %d", rc);

	g_backend.sector_size = info.size;
	g_backend.sector_count = 3U;

#if defined(CONFIG_ZDB_KV_BACKEND_ZMS)
	rc = zms_mount(&g_backend);
	zassert_equal(rc, 0, "zms mount failed: %d", rc);
#else
	rc = nvs_mount(&g_backend);
	zassert_equal(rc, 0, "nvs mount failed: %d", rc);
#endif
}

static void *kv_suite_setup(void)
{
	zdb_status_t rc;

	kv_backend_mount();
	g_cfg.kv_backend_fs = &g_backend;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "zdb_init failed: %d", rc);

	return NULL;
}

ZTEST_SUITE(kv_suite, NULL, kv_suite_setup, NULL, NULL, NULL);

ZTEST(kv_suite, test_kv_open_close_success)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);
	rc = zdb_kv_close(&kv);
	zassert_equal(rc, ZDB_OK, "close failed: %d", rc);
}

ZTEST(kv_suite, test_kv_open_invalid_args)
{
	zdb_kv_t kv;

	zassert_equal(zdb_kv_open(NULL, "ns", &kv), ZDB_ERR_INVAL);
	zassert_equal(zdb_kv_open(&g_db, NULL, &kv), ZDB_ERR_INVAL);
	zassert_equal(zdb_kv_open(&g_db, "ns", NULL), ZDB_ERR_INVAL);
	zassert_equal(zdb_kv_open(&g_db, "", &kv), ZDB_ERR_INVAL);
}

ZTEST(kv_suite, test_kv_set_get_roundtrip)
{
	zdb_kv_t kv;
	uint32_t set_value = 42U;
	uint32_t get_value = 0U;
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set(&kv, "rt_counter", &set_value, sizeof(set_value));
	zassert_equal(rc, ZDB_OK, "set failed: %d", rc);

	rc = zdb_kv_get(&kv, "rt_counter", &get_value, sizeof(get_value), &out_len);
	zassert_equal(rc, ZDB_OK, "get failed: %d", rc);
	zassert_equal(out_len, sizeof(set_value), "length mismatch: %zu", out_len);
	zassert_equal(get_value, set_value, "value mismatch: %u != %u", get_value, set_value);

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_get_not_found)
{
	zdb_kv_t kv;
	uint8_t buffer[32];
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_get(&kv, "nonexistent", buffer, sizeof(buffer), &out_len);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "expected NOT_FOUND, got %d", rc);

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_set_overwrite)
{
	zdb_kv_t kv;
	uint32_t value1 = 10U;
	uint32_t value2 = 20U;
	uint32_t readback = 0U;
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set(&kv, "ow_counter", &value1, sizeof(value1));
	zassert_equal(rc, ZDB_OK, "first set failed: %d", rc);

	rc = zdb_kv_set(&kv, "ow_counter", &value2, sizeof(value2));
	zassert_equal(rc, ZDB_OK, "second set failed: %d", rc);

	rc = zdb_kv_get(&kv, "ow_counter", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_OK, "get failed: %d", rc);
	zassert_equal(readback, value2, "not overwritten: %u != %u", readback, value2);

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_set_same_value_twice)
{
	zdb_kv_t kv;
	uint32_t value = 0xCAFEF00DU;
	uint32_t readback = 0U;
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set(&kv, "sv_key", &value, sizeof(value));
	zassert_equal(rc, ZDB_OK, "first set failed: %d", rc);

	/*
	 * Backends may skip the flash write entirely for byte-identical data
	 * (nvs_write returns 0); that is still a successful set.
	 */
	rc = zdb_kv_set(&kv, "sv_key", &value, sizeof(value));
	zassert_equal(rc, ZDB_OK, "identical rewrite failed: %d", rc);

	rc = zdb_kv_get(&kv, "sv_key", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_OK, "get failed: %d", rc);
	zassert_equal(readback, value, "value corrupted by identical rewrite");

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_delete_success)
{
	zdb_kv_t kv;
	uint32_t value = 42U;
	uint8_t buffer[32];
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set(&kv, "del_counter", &value, sizeof(value));
	zassert_equal(rc, ZDB_OK, "set failed: %d", rc);

	rc = zdb_kv_delete(&kv, "del_counter");
	zassert_equal(rc, ZDB_OK, "delete failed: %d", rc);

	rc = zdb_kv_get(&kv, "del_counter", buffer, sizeof(buffer), &out_len);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "expected NOT_FOUND after delete, got %d", rc);

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_delete_not_found)
{
	zdb_kv_t kv;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_delete(&kv, "never_existed");
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "expected NOT_FOUND, got %d", rc);

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_value_max_length)
{
	zdb_kv_t kv;
	uint8_t large_value[CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE];
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	(void)memset(large_value, 0xAA, sizeof(large_value));

	/* Well under the record overhead: must fit. */
	rc = zdb_kv_set(&kv, "large_key", large_value, 100U);
	zassert_equal(rc, ZDB_OK, "100-byte value failed: %d", rc);

	/* A value the size of the whole IO block can never fit with headers. */
	rc = zdb_kv_set(&kv, "large_key", large_value, sizeof(large_value));
	zassert_equal(rc, ZDB_ERR_NOMEM, "oversized value should be NOMEM, got %d", rc);

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_key_max_length)
{
	zdb_kv_t kv;
	char max_key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	char oversized_key[CONFIG_ZDB_MAX_KEY_LEN + 16U];
	uint32_t value = 123U;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	(void)memset(max_key, 'X', CONFIG_ZDB_MAX_KEY_LEN);
	max_key[CONFIG_ZDB_MAX_KEY_LEN] = '\0';
	rc = zdb_kv_set(&kv, max_key, &value, sizeof(value));
	zassert_equal(rc, ZDB_OK, "max-length key failed: %d", rc);

	(void)memset(oversized_key, 'Y', sizeof(oversized_key) - 1U);
	oversized_key[sizeof(oversized_key) - 1U] = '\0';
	rc = zdb_kv_set(&kv, oversized_key, &value, sizeof(value));
	zassert_equal(rc, ZDB_ERR_INVAL, "oversized key should be INVAL, got %d", rc);

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_namespace_isolation)
{
	zdb_kv_t kv_a;
	zdb_kv_t kv_b;
	uint32_t value_a = 111U;
	uint32_t value_b = 222U;
	uint32_t readback = 0U;
	size_t out_len = 0U;
	zdb_status_t rc;

	rc = zdb_kv_open(&g_db, "iso_ns_a", &kv_a);
	zassert_equal(rc, ZDB_OK, "open ns_a failed: %d", rc);
	rc = zdb_kv_open(&g_db, "iso_ns_b", &kv_b);
	zassert_equal(rc, ZDB_OK, "open ns_b failed: %d", rc);

	rc = zdb_kv_set(&kv_a, "shared_key", &value_a, sizeof(value_a));
	zassert_equal(rc, ZDB_OK, "set ns_a failed: %d", rc);
	rc = zdb_kv_set(&kv_b, "shared_key", &value_b, sizeof(value_b));
	zassert_equal(rc, ZDB_OK, "set ns_b failed: %d", rc);

	/* Each namespace keeps its own value for the same key name. */
	rc = zdb_kv_get(&kv_a, "shared_key", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_OK, "get ns_a failed: %d", rc);
	zassert_equal(readback, value_a, "ns_a value corrupted: %u != %u", readback, value_a);

	rc = zdb_kv_get(&kv_b, "shared_key", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_OK, "get ns_b failed: %d", rc);
	zassert_equal(readback, value_b, "ns_b value corrupted: %u != %u", readback, value_b);

	/* Deleting in one namespace must not touch the other. */
	rc = zdb_kv_delete(&kv_a, "shared_key");
	zassert_equal(rc, ZDB_OK, "delete ns_a failed: %d", rc);

	rc = zdb_kv_get(&kv_a, "shared_key", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "ns_a key should be gone, got %d", rc);

	rc = zdb_kv_get(&kv_b, "shared_key", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_OK, "ns_b lost its value after ns_a delete: %d", rc);
	zassert_equal(readback, value_b, "ns_b value corrupted after ns_a delete");

	zdb_kv_close(&kv_b);
	zdb_kv_close(&kv_a);
}

ZTEST(kv_suite, test_kv_ops_require_io_slab)
{
	zdb_kv_t kv;
	uint8_t out_buf[8];
	uint32_t value = 0x12345678U;
	size_t out_len = 0U;
	struct k_mem_slab *saved_slab = g_db.kv_io_slab;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);
	g_db.kv_io_slab = NULL;

	rc = zdb_kv_set(&kv, "k", &value, sizeof(value));
	zassert_equal(rc, ZDB_ERR_INVAL, "set without slab should be INVAL, got %d", rc);

	rc = zdb_kv_get(&kv, "k", out_buf, sizeof(out_buf), &out_len);
	zassert_equal(rc, ZDB_ERR_INVAL, "get without slab should be INVAL, got %d", rc);

	rc = zdb_kv_delete(&kv, "k");
	zassert_equal(rc, ZDB_ERR_INVAL, "delete without slab should be INVAL, got %d", rc);

	g_db.kv_io_slab = saved_slab;
	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_iter_lists_namespace_entries)
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

	rc = zdb_kv_open(&g_db, "iter_ns1", &kv_ns1);
	zassert_equal(rc, ZDB_OK, "open ns1 failed: %d", rc);
	rc = zdb_kv_open(&g_db, "iter_ns2", &kv_ns2);
	zassert_equal(rc, ZDB_OK, "open ns2 failed: %d", rc);

	rc = zdb_kv_set(&kv_ns1, "alpha", &value_a, sizeof(value_a));
	zassert_equal(rc, ZDB_OK, "set alpha failed: %d", rc);
	rc = zdb_kv_set(&kv_ns1, "beta", &value_b, sizeof(value_b));
	zassert_equal(rc, ZDB_OK, "set beta failed: %d", rc);
	rc = zdb_kv_set(&kv_ns2, "other", &value_other, sizeof(value_other));
	zassert_equal(rc, ZDB_OK, "set other failed: %d", rc);

	rc = zdb_kv_iter_open(&kv_ns1, &iter);
	zassert_equal(rc, ZDB_OK, "iter open failed: %d", rc);

	while ((rc = zdb_kv_iter_next(&iter, key, sizeof(key), &key_len,
				      &out_value, sizeof(out_value), &out_len)) == ZDB_OK) {
		zassert_equal(out_len, sizeof(uint32_t), "unexpected iter value length");
		zassert_true(key_len > 0U, "iterator key length should be > 0");
		if (strcmp(key, "alpha") == 0) {
			saw_alpha = true;
			zassert_equal(out_value, value_a, "alpha value mismatch");
		} else if (strcmp(key, "beta") == 0) {
			saw_beta = true;
			zassert_equal(out_value, value_b, "beta value mismatch");
		} else {
			zassert_unreachable("iterator returned foreign key: %s", key);
		}
	}

	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "iterator should end with NOT_FOUND");
	zassert_true(saw_alpha, "iterator did not return alpha");
	zassert_true(saw_beta, "iterator did not return beta");

	rc = zdb_kv_iter_close(&iter);
	zassert_equal(rc, ZDB_OK, "iter close failed: %d", rc);
	zdb_kv_close(&kv_ns2);
	zdb_kv_close(&kv_ns1);
}

ZTEST(kv_suite, test_kv_iter_skips_deleted_entries)
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

	rc = zdb_kv_open(&g_db, "iterdel_ns", &kv);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set(&kv, "keep", &keep_val, sizeof(keep_val));
	zassert_equal(rc, ZDB_OK, "set keep failed: %d", rc);
	rc = zdb_kv_set(&kv, "gone", &delete_val, sizeof(delete_val));
	zassert_equal(rc, ZDB_OK, "set gone failed: %d", rc);
	rc = zdb_kv_delete(&kv, "gone");
	zassert_equal(rc, ZDB_OK, "delete failed: %d", rc);

	rc = zdb_kv_iter_open(&kv, &iter);
	zassert_equal(rc, ZDB_OK, "iter open failed: %d", rc);

	while ((rc = zdb_kv_iter_next(&iter, key, sizeof(key), &key_len,
				      &out_value, sizeof(out_value), &out_len)) == ZDB_OK) {
		found++;
		zassert_equal(strcmp(key, "keep"), 0, "deleted key appeared: %s", key);
		zassert_equal(out_value, keep_val, "unexpected iterator value");
	}

	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "iterator should end with NOT_FOUND");
	zassert_equal(found, 1U, "expected exactly one iterated key, got %u", found);

	rc = zdb_kv_iter_close(&iter);
	zassert_equal(rc, ZDB_OK, "iter close failed: %d", rc);
	zdb_kv_close(&kv);
}

/*
 * Mirror of the production v2 record-ID derivation: FNV-1a over
 * "namespace \0 key", folded to 16 bits for NVS, 0 remapped to 1.
 */
static uint32_t test_record_id(const char *ns, const char *key)
{
	uint32_t hash = 0x811C9DC5u;
	const char *s;

	for (s = ns; (*s) != '\0'; s++) {
		hash ^= (uint8_t)(*s);
		hash *= 0x01000193u;
	}
	hash *= 0x01000193u; /* NUL separator byte (XOR with 0 is a no-op) */
	for (s = key; (*s) != '\0'; s++) {
		hash ^= (uint8_t)(*s);
		hash *= 0x01000193u;
	}

#if defined(CONFIG_ZDB_KV_BACKEND_NVS)
	hash &= 0xFFFFu;
#endif
	if (hash == 0U) {
		hash = 1U;
	}

	return hash;
}

#if defined(CONFIG_ZDB_KV_BACKEND_NVS)

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

static bool test_find_id16_collision(const char *ns, char key_a[5], char key_b[5])
{
	static uint32_t first_idx[65536];
	uint32_t i;
	const uint32_t total = 17U * 17U * 17U * 17U;

	for (i = 0U; i < ARRAY_SIZE(first_idx); i++) {
		first_idx[i] = UINT32_MAX;
	}

	for (i = 0U; i < total; i++) {
		char key[5];
		uint32_t h;

		test_key_from_index(i, key);
		h = test_record_id(ns, key);

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

ZTEST(kv_suite, test_kv_hash_collision_rejected)
{
	zdb_kv_t kv;
	char key_a[5];
	char key_b[5];
	uint32_t value_a = 0xAAAAAAAAU;
	uint32_t value_b = 0xBBBBBBBBU;
	uint32_t out_val = 0U;
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "collision_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	zassert_true(test_find_id16_collision("collision_ns", key_a, key_b),
		     "failed to find deterministic 16-bit ID collision");

	rc = zdb_kv_set(&kv, key_a, &value_a, sizeof(value_a));
	zassert_equal(rc, ZDB_OK, "set key_a failed: %d", rc);

	/* The colliding set is rejected; the stored record is untouched. */
	rc = zdb_kv_set(&kv, key_b, &value_b, sizeof(value_b));
	zassert_equal(rc, ZDB_ERR_COLLISION,
		      "colliding set should be COLLISION, got %d", rc);

	rc = zdb_kv_get(&kv, key_a, &out_val, sizeof(out_val), &out_len);
	zassert_equal(rc, ZDB_OK, "key_a lost after rejected collision: %d", rc);
	zassert_equal(out_val, value_a, "key_a payload corrupted");

	rc = zdb_kv_get(&kv, key_b, &out_val, sizeof(out_val), &out_len);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "key_b was never stored, got %d", rc);

	rc = zdb_kv_delete(&kv, key_b);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND,
		      "deleting the colliding key must not touch key_a, got %d", rc);

	rc = zdb_kv_get(&kv, key_a, &out_val, sizeof(out_val), &out_len);
	zassert_equal(rc, ZDB_OK, "key_a lost after key_b delete attempt: %d", rc);

	zdb_kv_close(&kv);
}
#endif /* CONFIG_ZDB_KV_BACKEND_NVS */

ZTEST(kv_suite, test_kv_old_format_record_ignored)
{
	zdb_kv_t kv;
	uint8_t v1_blob[16];
	uint32_t new_value = 0x600DF00DU;
	uint32_t out_val = 0U;
	size_t out_len = 0U;
	uint32_t id = test_record_id("test_ns", "legacy");
	const char *key = "legacy";
	size_t key_len = strlen(key);
	ssize_t wrc;
	zdb_status_t rc = zdb_kv_open(&g_db, "test_ns", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	/* Plant a v1-format record ([key_len][key][value]) in the slot. */
	v1_blob[0] = (uint8_t)key_len;
	(void)memcpy(&v1_blob[1], key, key_len);
	(void)memset(&v1_blob[1 + key_len], 0xEE, 4U);
#if defined(CONFIG_ZDB_KV_BACKEND_ZMS)
	wrc = zms_write(&g_backend, id, v1_blob, 1U + key_len + 4U);
#else
	wrc = nvs_write(&g_backend, (uint16_t)id, v1_blob, 1U + key_len + 4U);
#endif
	zassert_true(wrc > 0, "planting v1 record failed: %zd", wrc);

	/* Old-format records read as absent and are never deleted... */
	rc = zdb_kv_get(&kv, key, &out_val, sizeof(out_val), &out_len);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "v1 record should read as absent, got %d", rc);
	rc = zdb_kv_delete(&kv, key);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "v1 record should not be deletable, got %d", rc);

	/* ...but a set reclaims the slot with a v2 record. */
	rc = zdb_kv_set(&kv, key, &new_value, sizeof(new_value));
	zassert_equal(rc, ZDB_OK, "reclaiming set failed: %d", rc);

	rc = zdb_kv_get(&kv, key, &out_val, sizeof(out_val), &out_len);
	zassert_equal(rc, ZDB_OK, "get after reclaim failed: %d", rc);
	zassert_equal(out_val, new_value, "reclaimed value mismatch");

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_string_roundtrip)
{
	zdb_kv_t kv;
	char out[32];
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "ns_str", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set_str(&kv, "greeting", "hello");
	zassert_equal(rc, ZDB_OK, "set_str failed: %d", rc);

	rc = zdb_kv_get_str(&kv, "greeting", out, sizeof(out), &out_len);
	zassert_equal(rc, ZDB_OK, "get_str failed: %d", rc);
	zassert_str_equal(out, "hello", "value mismatch");
	zassert_equal(out_len, 5U, "expected length 5, got %zu", out_len);

	zdb_kv_close(&kv);
}

ZTEST(kv_suite, test_kv_string_empty)
{
	zdb_kv_t kv;
	char out[8];
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "ns_str_empty", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set_str(&kv, "empty", "");
	zassert_equal(rc, ZDB_OK, "storing an empty string failed: %d", rc);

	rc = zdb_kv_get_str(&kv, "empty", out, sizeof(out), &out_len);
	zassert_equal(rc, ZDB_OK, "get_str failed: %d", rc);
	zassert_equal(out_len, 0U, "expected length 0, got %zu", out_len);
	zassert_equal(out[0], '\0', "result not terminated");

	zdb_kv_close(&kv);
}

/* A short buffer must still yield a valid string, plus a way to spot the cut. */
ZTEST(kv_suite, test_kv_string_truncation_is_terminated)
{
	zdb_kv_t kv;
	char out[4];
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "ns_str_trunc", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set_str(&kv, "long", "abcdefgh");
	zassert_equal(rc, ZDB_OK, "set_str failed: %d", rc);

	rc = zdb_kv_get_str(&kv, "long", out, sizeof(out), &out_len);
	zassert_equal(rc, ZDB_OK, "get_str failed: %d", rc);
	zassert_equal(out[sizeof(out) - 1U], '\0', "truncated result not terminated");
	zassert_str_equal(out, "abc", "unexpected truncated prefix");
	zassert_true(out_len + 1U > sizeof(out), "truncation not detectable: %zu", out_len);

	zdb_kv_close(&kv);
}

/* get_str must cope with values written through the raw API. */
ZTEST(kv_suite, test_kv_string_reads_unterminated_blob)
{
	zdb_kv_t kv;
	const char raw[3] = {'x', 'y', 'z'};
	char out[16];
	size_t out_len = 0U;
	zdb_status_t rc = zdb_kv_open(&g_db, "ns_str_raw", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set(&kv, "raw", raw, sizeof(raw));
	zassert_equal(rc, ZDB_OK, "set failed: %d", rc);

	rc = zdb_kv_get_str(&kv, "raw", out, sizeof(out), &out_len);
	zassert_equal(rc, ZDB_OK, "get_str failed: %d", rc);
	zassert_str_equal(out, "xyz", "unterminated blob mishandled");
	zassert_equal(out_len, 3U, "expected length 3, got %zu", out_len);

	zdb_kv_close(&kv);
}

/* zdb_kv_set has always documented zero-length values as allowed. */
ZTEST(kv_suite, test_kv_set_zero_length_value)
{
	zdb_kv_t kv;
	uint8_t out[4];
	size_t out_len = 99U;
	zdb_status_t rc = zdb_kv_open(&g_db, "ns_zero", &kv);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set(&kv, "flag", out, 0U);
	zassert_equal(rc, ZDB_OK, "zero-length set rejected: %d", rc);

	rc = zdb_kv_get(&kv, "flag", out, sizeof(out), &out_len);
	zassert_equal(rc, ZDB_OK, "get failed: %d", rc);
	zassert_equal(out_len, 0U, "expected zero length, got %zu", out_len);

	/* NULL with a non-zero length is still a caller error. */
	zassert_equal(zdb_kv_set(&kv, "bad", NULL, 4U), ZDB_ERR_INVAL);

	zdb_kv_close(&kv);
}

#if defined(CONFIG_ZDB_KV_PERSIST_INDEX) && (CONFIG_ZDB_KV_PERSIST_INDEX)
/*
 * Iteration must survive a reboot. zdb_deinit()/zdb_init() frees and rebuilds
 * the heap index while the mounted backend keeps its contents, which is the
 * same starting point a reboot produces.
 *
 * These cases assert persistent-index behaviour, so they are compiled out when
 * CONFIG_ZDB_KV_PERSIST_INDEX=n selects session-only iteration.
 */
static void kv_reboot_cycle(void)
{
	zdb_status_t rc;

	(void)zdb_deinit(&g_db);
	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "re-init failed: %d", rc);
}

static size_t kv_count_keys(const char *ns, bool *out_saw_a, bool *out_saw_b)
{
	zdb_kv_t kv;
	zdb_kv_iter_t iter;
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	size_t key_len = 0U;
	uint32_t value = 0U;
	size_t value_len = 0U;
	size_t count = 0U;

	zassert_equal(zdb_kv_open(&g_db, ns, &kv), ZDB_OK, "open failed");
	zassert_equal(zdb_kv_iter_open(&kv, &iter), ZDB_OK, "iter open failed");

	while (zdb_kv_iter_next(&iter, key, sizeof(key), &key_len, &value, sizeof(value),
				&value_len) == ZDB_OK) {
		count++;
		if ((out_saw_a != NULL) && (strcmp(key, "alpha") == 0)) {
			*out_saw_a = true;
			zassert_equal(value, 1U, "alpha value mismatch: %u", value);
		}
		if ((out_saw_b != NULL) && (strcmp(key, "beta") == 0)) {
			*out_saw_b = true;
			zassert_equal(value, 2U, "beta value mismatch: %u", value);
		}
	}

	(void)zdb_kv_iter_close(&iter);
	(void)zdb_kv_close(&kv);
	return count;
}

ZTEST(kv_suite, test_kv_iteration_survives_reinit)
{
	zdb_kv_t kv;
	uint32_t a = 1U;
	uint32_t b = 2U;
	bool saw_a = false;
	bool saw_b = false;
	size_t count;

	zassert_equal(zdb_kv_open(&g_db, "ns_persist", &kv), ZDB_OK, "open failed");
	zassert_equal(zdb_kv_set(&kv, "alpha", &a, sizeof(a)), ZDB_OK, "set alpha failed");
	zassert_equal(zdb_kv_set(&kv, "beta", &b, sizeof(b)), ZDB_OK, "set beta failed");
	(void)zdb_kv_close(&kv);

	kv_reboot_cycle();

	count = kv_count_keys("ns_persist", &saw_a, &saw_b);
	zassert_true(saw_a, "alpha not enumerated after re-init");
	zassert_true(saw_b, "beta not enumerated after re-init");
	zassert_equal(count, 2U, "expected 2 keys, got %zu", count);
}

/* The index is per-namespace-filtered, so a reboot must not blur namespaces. */
ZTEST(kv_suite, test_kv_iteration_namespaces_stay_separate_after_reinit)
{
	zdb_kv_t kv;
	uint32_t v = 7U;

	zassert_equal(zdb_kv_open(&g_db, "ns_sep_one", &kv), ZDB_OK, "open failed");
	zassert_equal(zdb_kv_set(&kv, "only_one", &v, sizeof(v)), ZDB_OK, "set failed");
	(void)zdb_kv_close(&kv);

	zassert_equal(zdb_kv_open(&g_db, "ns_sep_two", &kv), ZDB_OK, "open failed");
	zassert_equal(zdb_kv_set(&kv, "only_two", &v, sizeof(v)), ZDB_OK, "set failed");
	(void)zdb_kv_close(&kv);

	kv_reboot_cycle();

	zassert_equal(kv_count_keys("ns_sep_one", NULL, NULL), 1U, "ns_sep_one leaked keys");
	zassert_equal(kv_count_keys("ns_sep_two", NULL, NULL), 1U, "ns_sep_two leaked keys");
}

/* A deleted key must not come back when the index is rebuilt. */
ZTEST(kv_suite, test_kv_deleted_key_absent_after_reinit)
{
	zdb_kv_t kv;
	uint32_t v = 5U;

	zassert_equal(zdb_kv_open(&g_db, "ns_del_persist", &kv), ZDB_OK, "open failed");
	zassert_equal(zdb_kv_set(&kv, "gone", &v, sizeof(v)), ZDB_OK, "set failed");
	zassert_equal(zdb_kv_set(&kv, "stays", &v, sizeof(v)), ZDB_OK, "set failed");
	zassert_equal(zdb_kv_delete(&kv, "gone"), ZDB_OK, "delete failed");
	(void)zdb_kv_close(&kv);

	kv_reboot_cycle();

	zassert_equal(kv_count_keys("ns_del_persist", NULL, NULL), 1U,
		      "deleted key resurrected by index rebuild");
}

/* Overwriting a value must not grow the index. */
ZTEST(kv_suite, test_kv_overwrite_does_not_duplicate_index_entry)
{
	zdb_kv_t kv;
	uint32_t v = 1U;

	zassert_equal(zdb_kv_open(&g_db, "ns_dup", &kv), ZDB_OK, "open failed");
	for (uint32_t i = 0U; i < 5U; i++) {
		v = i;
		zassert_equal(zdb_kv_set(&kv, "same", &v, sizeof(v)), ZDB_OK, "set %u failed", i);
	}
	(void)zdb_kv_close(&kv);

	kv_reboot_cycle();

	zassert_equal(kv_count_keys("ns_dup", NULL, NULL), 1U, "overwrite duplicated the key");
}
#endif /* CONFIG_ZDB_KV_PERSIST_INDEX */
