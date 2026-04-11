/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for KV iterator backend hydration.
 * Verifies that pre-existing NVS entries are discovered on first iter_open.
 *
 * Provides stub NVS functions (nvs_read/write/delete) so the test compiles
 * without CONFIG_NVS=y.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/fs/nvs.h>
#include <string.h>
#include <errno.h>

#include "zephyrdb.h"

/* ===== Mock NVS storage ===== */

#define MOCK_NVS_MAX_ENTRIES 32U
#define MOCK_NVS_MAX_DATA    256U

static struct {
	uint16_t id;
	uint8_t  data[MOCK_NVS_MAX_DATA];
	ssize_t  len;
	bool     used;
} g_mock_store[MOCK_NVS_MAX_ENTRIES];

static void mock_nvs_reset(void)
{
	(void)memset(g_mock_store, 0, sizeof(g_mock_store));
}

/* Stub NVS functions — linked instead of the real Zephyr NVS subsystem. */

ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t len)
{
	size_t i;

	ARG_UNUSED(fs);

	for (i = 0U; i < MOCK_NVS_MAX_ENTRIES; i++) {
		if (g_mock_store[i].used && (g_mock_store[i].id == id)) {
			size_t copy = MIN(len, (size_t)g_mock_store[i].len);

			(void)memcpy(data, g_mock_store[i].data, copy);
			return g_mock_store[i].len;
		}
	}

	return -ENOENT;
}

ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len)
{
	size_t i;

	ARG_UNUSED(fs);

	/* Update existing slot. */
	for (i = 0U; i < MOCK_NVS_MAX_ENTRIES; i++) {
		if (g_mock_store[i].used && (g_mock_store[i].id == id)) {
			(void)memcpy(g_mock_store[i].data, data, len);
			g_mock_store[i].len = (ssize_t)len;
			return (ssize_t)len;
		}
	}

	/* Allocate new slot. */
	for (i = 0U; i < MOCK_NVS_MAX_ENTRIES; i++) {
		if (!g_mock_store[i].used) {
			g_mock_store[i].id = id;
			(void)memcpy(g_mock_store[i].data, data, len);
			g_mock_store[i].len = (ssize_t)len;
			g_mock_store[i].used = true;
			return (ssize_t)len;
		}
	}

	return -ENOSPC;
}

int nvs_delete(struct nvs_fs *fs, uint16_t id)
{
	size_t i;

	ARG_UNUSED(fs);

	for (i = 0U; i < MOCK_NVS_MAX_ENTRIES; i++) {
		if (g_mock_store[i].used && (g_mock_store[i].id == id)) {
			g_mock_store[i].used = false;
			return 0;
		}
	}

	return -ENOENT;
}

/* ===== Test helpers ===== */

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

/*
 * Plant a v2-format entry directly into the mock NVS store, bypassing
 * ZephyrDB APIs.  Simulates data written by a previous boot cycle.
 */
static void plant_v2_entry(const char *key, const void *value, size_t value_len)
{
	uint16_t id = test_fnv1a16(key);
	size_t key_len = strlen(key);
	size_t i;

	for (i = 0U; i < MOCK_NVS_MAX_ENTRIES; i++) {
		if (!g_mock_store[i].used) {
			g_mock_store[i].id = id;
			g_mock_store[i].data[0] = (uint8_t)key_len;
			(void)memcpy(&g_mock_store[i].data[1], key, key_len);
			(void)memcpy(&g_mock_store[i].data[1U + key_len], value, value_len);
			g_mock_store[i].len = (ssize_t)(1U + key_len + value_len);
			g_mock_store[i].used = true;
			return;
		}
	}

	zassert_unreachable("Mock NVS store full");
}

/* ===== Database instance ===== */

K_MEM_SLAB_DEFINE_STATIC(g_core_slab, 128, 16, 4);
K_MEM_SLAB_DEFINE_STATIC(g_cursor_slab, 96, 8, 4);
K_MEM_SLAB_DEFINE_STATIC(g_kv_io_slab, 128, 8, 4);

static struct nvs_fs g_fake_nvs; /* contents unused; mock ignores fs pointer */

static zdb_t g_db = {
	.core_slab = &g_core_slab,
	.cursor_slab = &g_cursor_slab,
	.kv_io_slab = &g_kv_io_slab,
};

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = &g_fake_nvs,
	.lfs_mount_point = NULL,
	.work_q = &k_sys_work_q,
};

/* ===== Fixture ===== */

static void setup(void *fixture)
{
	zdb_status_t rc;

	ARG_UNUSED(fixture);
	mock_nvs_reset();

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);
}

static void teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	zdb_deinit(&g_db);
}

/* ===== Tests ===== */

/*
 * Pre-existing v2 entries planted directly into mock NVS (simulating data
 * written in a previous session) must be discovered by the first iter_open.
 */
ZTEST(kv_hydrate, test_iter_discovers_preexisting_entries)
{
	zdb_kv_t kv;
	zdb_kv_iter_t iter;
	zdb_status_t rc;
	uint32_t planted_a = 0xAAU;
	uint32_t planted_b = 0xBBU;
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	uint32_t out_val = 0U;
	size_t key_len = 0U;
	size_t val_len = 0U;
	bool saw_a = false;
	bool saw_b = false;
	unsigned int count = 0U;

	/* Plant entries before any ZephyrDB API calls. */
	plant_v2_entry("temp", &planted_a, sizeof(planted_a));
	plant_v2_entry("hum", &planted_b, sizeof(planted_b));

	rc = zdb_kv_open(&g_db, "sensors", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open: %d", rc);

	rc = zdb_kv_iter_open(&kv, &iter);
	zassert_equal(rc, ZDB_OK, "iter_open: %d", rc);

	while (zdb_kv_iter_next(&iter, key, sizeof(key), &key_len,
				&out_val, sizeof(out_val), &val_len) == ZDB_OK) {
		count++;
		if (strcmp(key, "temp") == 0) {
			saw_a = true;
			zassert_equal(out_val, planted_a, "temp value mismatch");
		} else if (strcmp(key, "hum") == 0) {
			saw_b = true;
			zassert_equal(out_val, planted_b, "hum value mismatch");
		} else {
			zassert_unreachable("unexpected key: %s", key);
		}
	}

	zassert_equal(count, 2U, "expected 2 hydrated entries, got %u", count);
	zassert_true(saw_a, "did not discover 'temp'");
	zassert_true(saw_b, "did not discover 'hum'");

	zdb_kv_iter_close(&iter);
	zdb_kv_close(&kv);
}

/*
 * Runtime zdb_kv_set entries and pre-existing hydrated entries must both
 * appear in the same iteration pass.
 */
ZTEST(kv_hydrate, test_hydrated_and_runtime_entries_coexist)
{
	zdb_kv_t kv;
	zdb_kv_iter_t iter;
	zdb_status_t rc;
	uint32_t old_val = 1U;
	uint32_t new_val = 2U;
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	uint32_t out_val = 0U;
	size_t key_len = 0U;
	size_t val_len = 0U;
	bool saw_old = false;
	bool saw_new = false;
	unsigned int count = 0U;

	/* Simulate pre-existing entry. */
	plant_v2_entry("old_key", &old_val, sizeof(old_val));

	rc = zdb_kv_open(&g_db, "mixed", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open: %d", rc);

	/* Write a new entry at runtime. */
	rc = zdb_kv_set(&kv, "new_key", &new_val, sizeof(new_val));
	zassert_equal(rc, ZDB_OK, "kv_set: %d", rc);

	/* First iter_open triggers hydration; both entries should appear. */
	rc = zdb_kv_iter_open(&kv, &iter);
	zassert_equal(rc, ZDB_OK, "iter_open: %d", rc);

	while (zdb_kv_iter_next(&iter, key, sizeof(key), &key_len,
				&out_val, sizeof(out_val), &val_len) == ZDB_OK) {
		count++;
		if (strcmp(key, "old_key") == 0) {
			saw_old = true;
			zassert_equal(out_val, old_val, "old_key value mismatch");
		} else if (strcmp(key, "new_key") == 0) {
			saw_new = true;
			zassert_equal(out_val, new_val, "new_key value mismatch");
		} else {
			zassert_unreachable("unexpected key: %s", key);
		}
	}

	zassert_equal(count, 2U, "expected 2 entries, got %u", count);
	zassert_true(saw_old, "did not find hydrated old_key");
	zassert_true(saw_new, "did not find runtime new_key");

	zdb_kv_iter_close(&iter);
	zdb_kv_close(&kv);
}

/*
 * Non-ZephyrDB NVS entries (key doesn't hash-back to stored ID) must be
 * ignored during hydration.
 */
ZTEST(kv_hydrate, test_hydration_ignores_foreign_entries)
{
	zdb_kv_t kv;
	zdb_kv_iter_t iter;
	zdb_status_t rc;
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	uint32_t out_val = 0U;
	size_t key_len = 0U;
	size_t val_len = 0U;
	size_t i;

	/*
	 * Write a raw entry at an arbitrary NVS ID that does NOT match the
	 * FNV1a16 hash of the stored key.  Hydration must skip it.
	 */
	uint8_t foreign[8];
	size_t foreign_key_len = 3U;

	foreign[0] = (uint8_t)foreign_key_len;
	(void)memcpy(&foreign[1], "xyz", foreign_key_len);
	foreign[4] = 0xFFU;

	/* Pick an ID that "xyz" does NOT hash to. */
	uint16_t real_id = test_fnv1a16("xyz");
	uint16_t fake_id = (real_id == 1U) ? 2U : (uint16_t)(real_id - 1U);

	for (i = 0U; i < MOCK_NVS_MAX_ENTRIES; i++) {
		if (!g_mock_store[i].used) {
			g_mock_store[i].id = fake_id;
			(void)memcpy(g_mock_store[i].data, foreign, 5U);
			g_mock_store[i].len = 5;
			g_mock_store[i].used = true;
			break;
		}
	}

	rc = zdb_kv_open(&g_db, "ns", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open: %d", rc);

	rc = zdb_kv_iter_open(&kv, &iter);
	zassert_equal(rc, ZDB_OK, "iter_open: %d", rc);

	rc = zdb_kv_iter_next(&iter, key, sizeof(key), &key_len,
			      &out_val, sizeof(out_val), &val_len);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "foreign entry should not be yielded");

	zdb_kv_iter_close(&iter);
	zdb_kv_close(&kv);
}

ZTEST_SUITE(kv_hydrate, NULL, NULL, setup, teardown, NULL);
