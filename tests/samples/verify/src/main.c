/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sample-workflow verification: runs the critical path of each sample
 * (kv_basic, ts_basic, doc_basic) against real backends so a change that
 * would break a sample's happy path fails here.
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

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
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

static void verify_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zms_backend_mount_once();
	g_cfg.kv_backend_fs = &g_zms;
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void verify_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(sample_verify, NULL, NULL, verify_before, verify_after, NULL);

/*
 * Mirrors samples/kv_basic: open -> set -> get -> delete -> gone.
 */
ZTEST(sample_verify, test_sample_kv_basic_workflow)
{
	zdb_kv_t kv;
	uint32_t boot_count = 5U;
	uint32_t readback = 0U;
	size_t out_len = 0U;
	zdb_status_t rc;

	rc = zdb_kv_open(&g_db, "app", &kv);
	zassert_equal(rc, ZDB_OK, "kv open failed: %d", rc);

	zassert_equal(zdb_kv_set(&kv, "boot_count", &boot_count, sizeof(boot_count)), ZDB_OK);

	rc = zdb_kv_get(&kv, "boot_count", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_OK, "kv get failed: %d", rc);
	zassert_equal(readback, boot_count, "boot count mismatch");
	zassert_equal(out_len, sizeof(boot_count), "length mismatch");

	zassert_equal(zdb_kv_delete(&kv, "boot_count"), ZDB_OK);

	rc = zdb_kv_get(&kv, "boot_count", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "expected NOT_FOUND after delete, got %d", rc);

	zdb_kv_close(&kv);
}

/*
 * Mirrors samples/ts_basic: open -> append -> flush -> aggregate.
 */
ZTEST(sample_verify, test_sample_ts_basic_workflow)
{
	zdb_ts_t stream;
	zdb_ts_agg_result_t result = {0};
	zdb_status_t rc;

	rc = zdb_ts_open(&g_db, "sv_metrics", &stream);
	zassert_equal(rc, ZDB_OK, "ts open failed: %d", rc);

	for (int i = 0; i < 5; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = 1000U + ((uint64_t)i * 1000U),
			.value = 100 + (i * 10),
		};
		rc = zdb_ts_append_i64(&stream, &sample);
		zassert_equal(rc, ZDB_OK, "ts append failed: %d", rc);
	}

	rc = zdb_ts_flush_sync(&stream, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "ts flush failed: %d", rc);

	/* Values 100..140 step 10 -> average 120. */
	rc = zdb_ts_query_aggregate(&stream, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_AVG, &result);
	zassert_equal(rc, ZDB_OK, "aggregate failed: %d", rc);
	zassert_equal(result.points, 5U, "expected 5 points, got %u", result.points);
	zassert_within(result.value, 120.0, 0.0001, "average mismatch");

	zdb_ts_close(&stream);
}

/*
 * Mirrors samples/doc_basic: create -> set fields -> save -> get fields.
 */
ZTEST(sample_verify, test_sample_doc_basic_workflow)
{
	zdb_doc_t doc;
	const char *name = NULL;
	int64_t age = 0;
	bool active = false;
	zdb_status_t rc;

	rc = zdb_doc_create(&g_db, "sv_users", "u1", &doc);
	zassert_equal(rc, ZDB_OK, "doc create failed: %d", rc);

	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Ada"), ZDB_OK);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 30), ZDB_OK);
	zassert_equal(zdb_doc_field_set_bool(&doc, "active", true), ZDB_OK);

	rc = zdb_doc_save(&doc);
	zassert_equal(rc, ZDB_OK, "doc save failed: %d", rc);

	zassert_equal(zdb_doc_field_get_string(&doc, "name", &name), ZDB_OK);
	zassert_str_equal(name, "Ada", "name mismatch");
	zassert_equal(zdb_doc_field_get_i64(&doc, "age", &age), ZDB_OK);
	zassert_equal(age, 30, "age mismatch: %lld", (long long)age);
	zassert_equal(zdb_doc_field_get_bool(&doc, "active", &active), ZDB_OK);
	zassert_true(active, "active flag mismatch");

	zdb_doc_close(&doc);
}
