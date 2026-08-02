/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the KV defaults table applied by zdb_init(), running against a
 * real ZMS backend on the native_sim flash simulator.
 *
 * The backend is mounted once and persists across zdb_deinit()/zdb_init()
 * cycles within the run, which is what lets these tests stand in for a
 * reboot: re-initializing with a different defaults table is the firmware
 * upgrade case.
 *
 * Each test uses its own namespace so stored keys do not leak between them.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/zms.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>

#include "zephyrdb.h"

static struct zms_fs g_zms;
static bool g_zms_mounted;

static const uint32_t g_default_baud = 115200U;
static const uint32_t g_default_retries = 3U;

static const zdb_kv_default_t g_defaults[] = {
	{ .namespace_name = "cfg", .key = "baud",    .value = &g_default_baud,    .value_len = sizeof(g_default_baud) },
	{ .namespace_name = "cfg", .key = "retries", .value = &g_default_retries, .value_len = sizeof(g_default_retries) },
};

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = NULL,
	.work_q = &k_sys_work_q,
	.kv_defaults = g_defaults,
	.kv_default_count = ARRAY_SIZE(g_defaults),
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void zms_backend_mount_once(void)
{
	struct flash_pages_info info;
	int rc;

	if (g_zms_mounted) {
		return;
	}

	g_zms.flash_device = PARTITION_DEVICE(storage_partition);
	zassert_true(device_is_ready(g_zms.flash_device), "storage device not ready");

	g_zms.offset = PARTITION_OFFSET(storage_partition);
	rc = flash_get_page_info_by_offs(g_zms.flash_device, g_zms.offset, &info);
	zassert_equal(rc, 0, "flash page info failed: %d", rc);

	g_zms.sector_size = info.size;
	g_zms.sector_count = 3U;

	rc = zms_mount(&g_zms);
	zassert_equal(rc, 0, "zms mount failed: %d", rc);
	g_zms_mounted = true;
}

static void defaults_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zms_backend_mount_once();
	g_cfg.kv_backend_fs = &g_zms;
}

static void defaults_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(kv_defaults_suite, NULL, NULL, defaults_before, defaults_after, NULL);

static uint32_t read_u32(const char *ns, const char *key, zdb_status_t *out_rc)
{
	zdb_kv_t kv;
	uint32_t value = 0U;
	size_t len = 0U;

	*out_rc = zdb_kv_open(&g_db, ns, &kv);
	if (*out_rc != ZDB_OK) {
		return 0U;
	}

	*out_rc = zdb_kv_get(&kv, key, &value, sizeof(value), &len);
	(void)zdb_kv_close(&kv);
	return value;
}

static void write_u32(const char *ns, const char *key, uint32_t value)
{
	zdb_kv_t kv;

	zassert_equal(zdb_kv_open(&g_db, ns, &kv), ZDB_OK, "open failed");
	zassert_equal(zdb_kv_set(&kv, key, &value, sizeof(value)), ZDB_OK, "set failed");
	(void)zdb_kv_close(&kv);
}

/* First boot: nothing stored, so every default is written. */
ZTEST(kv_defaults_suite, test_defaults_seeded_on_init)
{
	zdb_status_t rc;
	uint32_t value;

	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "init failed");

	value = read_u32("cfg", "baud", &rc);
	zassert_equal(rc, ZDB_OK, "baud not seeded: %d", rc);
	zassert_equal(value, g_default_baud, "baud mismatch: %u", value);

	value = read_u32("cfg", "retries", &rc);
	zassert_equal(rc, ZDB_OK, "retries not seeded: %d", rc);
	zassert_equal(value, g_default_retries, "retries mismatch: %u", value);
}

/*
 * The upgrade case: a value the application changed must not be reverted by a
 * later init, which is what makes the pass safe to run on every boot.
 */
ZTEST(kv_defaults_suite, test_modified_value_survives_reinit)
{
	zdb_status_t rc;
	uint32_t value;

	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "init failed");
	write_u32("cfg", "baud", 9600U);

	(void)zdb_deinit(&g_db);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "re-init failed");

	value = read_u32("cfg", "baud", &rc);
	zassert_equal(rc, ZDB_OK, "baud missing after re-init: %d", rc);
	zassert_equal(value, 9600U, "user value reverted to default: %u", value);
}

/* A firmware update that adds a default seeds only the new key. */
ZTEST(kv_defaults_suite, test_new_default_added_on_upgrade)
{
	static const uint32_t added = 42U;
	static const zdb_kv_default_t upgraded[] = {
		{ .namespace_name = "up", .key = "old", .value = &g_default_baud,
		  .value_len = sizeof(g_default_baud) },
		{ .namespace_name = "up", .key = "new", .value = &added,
		  .value_len = sizeof(added) },
	};
	zdb_cfg_t v1_cfg = g_cfg;
	zdb_cfg_t v2_cfg = g_cfg;
	zdb_status_t rc;
	uint32_t value;

	/* v1 firmware ships only "old". */
	v1_cfg.kv_defaults = upgraded;
	v1_cfg.kv_default_count = 1U;
	zassert_equal(zdb_init(&g_db, &v1_cfg), ZDB_OK, "v1 init failed");
	write_u32("up", "old", 7U);
	(void)zdb_deinit(&g_db);

	/* v2 adds "new" and must leave the modified "old" alone. */
	v2_cfg.kv_defaults = upgraded;
	v2_cfg.kv_default_count = ARRAY_SIZE(upgraded);
	zassert_equal(zdb_init(&g_db, &v2_cfg), ZDB_OK, "v2 init failed");

	value = read_u32("up", "old", &rc);
	zassert_equal(rc, ZDB_OK, "old key lost: %d", rc);
	zassert_equal(value, 7U, "old key overwritten by upgrade: %u", value);

	value = read_u32("up", "new", &rc);
	zassert_equal(rc, ZDB_OK, "new default not seeded: %d", rc);
	zassert_equal(value, added, "new default mismatch: %u", value);
}

/*
 * Documented consequence: a deleted key is indistinguishable from one that was
 * never written, so the next pass re-seeds it.
 */
ZTEST(kv_defaults_suite, test_deleted_key_is_reseeded)
{
	static const zdb_kv_default_t del_defaults[] = {
		{ .namespace_name = "del", .key = "k", .value = &g_default_baud,
		  .value_len = sizeof(g_default_baud) },
	};
	zdb_cfg_t cfg = g_cfg;
	zdb_kv_t kv;
	zdb_status_t rc;
	uint32_t value;

	cfg.kv_defaults = del_defaults;
	cfg.kv_default_count = ARRAY_SIZE(del_defaults);

	zassert_equal(zdb_init(&g_db, &cfg), ZDB_OK, "init failed");
	zassert_equal(zdb_kv_open(&g_db, "del", &kv), ZDB_OK, "open failed");
	zassert_equal(zdb_kv_delete(&kv, "k"), ZDB_OK, "delete failed");
	(void)zdb_kv_close(&kv);

	(void)zdb_deinit(&g_db);
	zassert_equal(zdb_init(&g_db, &cfg), ZDB_OK, "re-init failed");

	value = read_u32("del", "k", &rc);
	zassert_equal(rc, ZDB_OK, "deleted key not re-seeded: %d", rc);
	zassert_equal(value, g_default_baud, "re-seeded value mismatch: %u", value);
}

/* The pass is callable directly, without re-initializing. */
ZTEST(kv_defaults_suite, test_defaults_apply_standalone)
{
	static const uint32_t manual = 55U;
	static const zdb_kv_default_t manual_defaults[] = {
		{ .namespace_name = "man", .key = "k", .value = &manual,
		  .value_len = sizeof(manual) },
	};
	zdb_cfg_t cfg = g_cfg;
	zdb_status_t rc;
	uint32_t value;

	/* Init with no table, so nothing is seeded automatically. */
	cfg.kv_defaults = NULL;
	cfg.kv_default_count = 0U;
	zassert_equal(zdb_init(&g_db, &cfg), ZDB_OK, "init failed");

	value = read_u32("man", "k", &rc);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "key present before applying: %d", rc);

	/* Point the live config at a table and apply it explicitly. */
	cfg.kv_defaults = manual_defaults;
	cfg.kv_default_count = ARRAY_SIZE(manual_defaults);
	zassert_equal(zdb_init(&g_db, &cfg), ZDB_OK, "re-init failed");

	zassert_equal(zdb_kv_defaults_apply(&g_db), ZDB_OK, "explicit apply failed");

	value = read_u32("man", "k", &rc);
	zassert_equal(rc, ZDB_OK, "key not seeded: %d", rc);
	zassert_equal(value, manual, "value mismatch: %u", value);
}

/* No table configured must leave init behaviour untouched. */
ZTEST(kv_defaults_suite, test_init_without_defaults_is_unchanged)
{
	zdb_cfg_t cfg = g_cfg;

	cfg.kv_defaults = NULL;
	cfg.kv_default_count = 0U;

	zassert_equal(zdb_init(&g_db, &cfg), ZDB_OK, "init without defaults failed");
	zassert_equal(zdb_kv_defaults_apply(&g_db), ZDB_OK, "apply with no table should be a no-op");
}

/* A malformed entry is reported without stopping the rest of the table. */
ZTEST(kv_defaults_suite, test_invalid_entry_reported_but_others_applied)
{
	static const uint32_t good = 11U;
	static const zdb_kv_default_t mixed[] = {
		{ .namespace_name = NULL,  .key = "bad",  .value = &good, .value_len = sizeof(good) },
		{ .namespace_name = "mix", .key = "good", .value = &good, .value_len = sizeof(good) },
	};
	zdb_cfg_t cfg = g_cfg;
	zdb_status_t rc;
	uint32_t value;

	cfg.kv_defaults = mixed;
	cfg.kv_default_count = ARRAY_SIZE(mixed);

	/* init surfaces the bad entry... */
	zassert_equal(zdb_init(&g_db, &cfg), ZDB_ERR_INVAL, "invalid entry not reported");

	/* ...and the valid one was still written. */
	value = read_u32("mix", "good", &rc);
	zassert_equal(rc, ZDB_OK, "valid entry skipped after an invalid one: %d", rc);
	zassert_equal(value, good, "value mismatch: %u", value);
}
