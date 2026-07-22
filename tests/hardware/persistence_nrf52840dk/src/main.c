/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware persistence tests for nRF52840 DK: NVS-backed KV on
 * storage_partition and LittleFS-backed TS/DOC on the (otherwise unused)
 * slot1 partition, automounted at /lfs via boards/nrf52840dk_nrf52840.overlay.
 *
 * CI builds this suite (build_only); to execute on a board:
 *   twister -T tests/hardware -p nrf52840dk/nrf52840 \
 *           --device-testing --device-serial /dev/ttyACM0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>

#include "zephyrdb.h"

static struct nvs_fs g_nvs;
static bool g_nvs_mounted;

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void nvs_backend_mount_once(void)
{
	struct flash_pages_info info;
	int rc;

	if (g_nvs_mounted) {
		return;
	}

	g_nvs.flash_device = PARTITION_DEVICE(storage_partition);
	zassert_true(device_is_ready(g_nvs.flash_device), "storage device not ready");

	g_nvs.offset = PARTITION_OFFSET(storage_partition);
	rc = flash_get_page_info_by_offs(g_nvs.flash_device, g_nvs.offset, &info);
	zassert_equal(rc, 0, "flash page info failed: %d", rc);

	g_nvs.sector_size = info.size;
	g_nvs.sector_count = 3U;

	rc = nvs_mount(&g_nvs);
	zassert_equal(rc, 0, "nvs mount failed: %d", rc);
	g_nvs_mounted = true;
}

static void hw_before(void *fixture)
{
	ARG_UNUSED(fixture);
	nvs_backend_mount_once();
	g_cfg.kv_backend_fs = &g_nvs;
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void hw_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(hw_persistence, NULL, NULL, hw_before, hw_after, NULL);

/*
 * KV value survives a namespace close/reopen cycle on real NVS.
 * (Power-cycle persistence needs a manual two-run check; this covers the
 * write-through path.)
 */
ZTEST(hw_persistence, test_kv_nvs_persistence)
{
	zdb_kv_t kv;
	uint32_t original_value = 0xDEADBEEFU;
	uint32_t readback = 0U;
	size_t len = 0U;
	zdb_status_t rc;

	rc = zdb_kv_open(&g_db, "persistent", &kv);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_kv_set(&kv, "magic_number", &original_value, sizeof(original_value));
	zassert_equal(rc, ZDB_OK, "set failed: %d", rc);

	zassert_equal(zdb_kv_close(&kv), ZDB_OK);

	rc = zdb_kv_open(&g_db, "persistent", &kv);
	zassert_equal(rc, ZDB_OK, "reopen failed: %d", rc);

	rc = zdb_kv_get(&kv, "magic_number", &readback, sizeof(readback), &len);
	zassert_equal(rc, ZDB_OK, "get after reopen failed: %d", rc);
	zassert_equal(readback, original_value, "value did not persist: 0x%x", readback);

	zdb_kv_close(&kv);
}

/*
 * Multiple keys in one namespace all remain readable.
 */
ZTEST(hw_persistence, test_kv_nvs_multiple_keys)
{
	zdb_kv_t kv;
	const struct {
		const char *key;
		uint32_t value;
	} kvs[] = {
		{"key1", 0x11111111U},
		{"key2", 0x22222222U},
		{"key3", 0x33333333U},
	};
	zdb_status_t rc;

	rc = zdb_kv_open(&g_db, "multi", &kv);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	for (size_t i = 0U; i < ARRAY_SIZE(kvs); i++) {
		rc = zdb_kv_set(&kv, kvs[i].key, &kvs[i].value, sizeof(kvs[i].value));
		zassert_equal(rc, ZDB_OK, "set %zu failed: %d", i, rc);
	}

	for (size_t i = 0U; i < ARRAY_SIZE(kvs); i++) {
		uint32_t readback = 0U;
		size_t len = 0U;

		rc = zdb_kv_get(&kv, kvs[i].key, &readback, sizeof(readback), &len);
		zassert_equal(rc, ZDB_OK, "get %zu failed: %d", i, rc);
		zassert_equal(readback, kvs[i].value, "key %zu corrupted", i);
	}

	zdb_kv_close(&kv);
}

/*
 * TS append-log grows across many samples and flushes cleanly.
 */
ZTEST(hw_persistence, test_ts_littlefs_append_log)
{
	zdb_ts_t stream;
	zdb_ts_agg_result_t result = {0};
	zdb_status_t rc;

	rc = zdb_ts_open(&g_db, "metrics_hw", &stream);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	for (int i = 0; i < 100; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = 1000U + ((uint64_t)i * 1000U),
			.value = 1000 + i,
		};
		rc = zdb_ts_append_i64(&stream, &sample);
		zassert_equal(rc, ZDB_OK, "append %d failed: %d", i, rc);
	}

	rc = zdb_ts_flush_sync(&stream, K_SECONDS(5));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&stream, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "count failed: %d", rc);
	zassert_equal(result.points, 100U, "expected 100 points, got %u", result.points);

	zassert_not_equal(zdb_health(&g_db), ZDB_HEALTH_FAULT, "health FAULT after TS ops");

	zdb_ts_close(&stream);
}

/*
 * Recovery on a cleanly flushed stream truncates nothing.
 */
ZTEST(hw_persistence, test_ts_littlefs_recovery_clean)
{
	zdb_ts_t stream;
	size_t truncated = 42U;
	zdb_status_t rc;

	rc = zdb_ts_open(&g_db, "recovery_hw", &stream);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	for (int i = 0; i < 10; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = 1000U + ((uint64_t)i * 100U),
			.value = 500 + i,
		};
		rc = zdb_ts_append_i64(&stream, &sample);
		zassert_equal(rc, ZDB_OK, "append %d failed: %d", i, rc);
	}

	rc = zdb_ts_flush_sync(&stream, K_SECONDS(3));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_recover_stream(&stream, &truncated);
	zassert_equal(rc, ZDB_OK, "recover failed: %d", rc);
	zassert_equal(truncated, 0U, "clean stream truncated %zu bytes", truncated);

	zdb_ts_close(&stream);
}

/*
 * A multi-field document survives a save/reopen cycle on LittleFS.
 */
ZTEST(hw_persistence, test_doc_littlefs_save_large)
{
	zdb_doc_t doc;
	const char *large_string =
		"This is a reasonably long string to test field persistence in LittleFS";
	const char *retrieved = NULL;
	zdb_status_t rc;

	rc = zdb_doc_create(&g_db, "hw_docs", "large_doc", &doc);
	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);

	zassert_equal(zdb_doc_field_set_string(&doc, "description", large_string), ZDB_OK);

	for (int i = 0; i < 10; i++) {
		char field_name[20];

		snprintf(field_name, sizeof(field_name), "value_%d", i);
		zassert_equal(zdb_doc_field_set_i64(&doc, field_name, 1000 + i), ZDB_OK);
	}

	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");
	zassert_equal(zdb_doc_close(&doc), ZDB_OK);

	rc = zdb_doc_open(&g_db, "hw_docs", "large_doc", &doc);
	zassert_equal(rc, ZDB_OK, "reopen failed: %d", rc);

	zassert_equal(zdb_doc_field_get_string(&doc, "description", &retrieved), ZDB_OK);
	zassert_str_equal(retrieved, large_string, "description not persisted");

	for (int i = 0; i < 10; i++) {
		char field_name[20];
		int64_t value = 0;

		snprintf(field_name, sizeof(field_name), "value_%d", i);
		zassert_equal(zdb_doc_field_get_i64(&doc, field_name, &value), ZDB_OK);
		zassert_equal(value, 1000 + i, "field %d not persisted", i);
	}

	zdb_doc_close(&doc);
}
