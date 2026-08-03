/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for ZephyrDB KV event emitter/listener feature, running
 * against a real ZMS backend on the native_sim flash simulator so the
 * success paths emit events with real statuses.
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

/* ===== Listener capture harness ===== */

static zdb_kv_event_t g_last_event;
static uint32_t g_event_count;
static uint32_t g_index_full_count;
static zdb_kv_event_t g_last_index_full;

static void capture_listener(const zdb_kv_event_t *event, void *user_ctx)
{
	ARG_UNUSED(user_ctx);

	if (event == NULL) {
		return;
	}

	if (event->type == ZDB_EVENT_KV_INDEX_FULL) {
		g_index_full_count++;
		g_last_index_full = *event;
		return;
	}

	g_last_event = *event;
	g_event_count++;
}

/* Second listener to verify multi-listener dispatch. */
static uint32_t g_second_count;

static void second_listener(const zdb_kv_event_t *event, void *user_ctx)
{
	ARG_UNUSED(event);
	ARG_UNUSED(user_ctx);
	g_second_count++;
}

static const zdb_event_listener_t g_listeners[] = {
	/* Slot with null notify: must not crash. */
	{ .notify = NULL,             .user_ctx = NULL },
	{ .notify = capture_listener, .user_ctx = NULL },
	{ .notify = second_listener,  .user_ctx = NULL },
};

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = NULL,
	.work_q = &k_sys_work_q,
	.event_listeners = g_listeners,
	.event_listener_count = ARRAY_SIZE(g_listeners),
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

/* ===== Test fixture ===== */

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

static void setup(void *fixture)
{
	ARG_UNUSED(fixture);
	zms_backend_mount_once();
	(void)memset(&g_last_event, 0, sizeof(g_last_event));
	(void)memset(&g_last_index_full, 0, sizeof(g_last_index_full));
	g_event_count = 0U;
	g_second_count = 0U;
	g_index_full_count = 0U;
}

/* ===== Tests ===== */

/*
 * No events emitted when there is no backend (open fails before backend work).
 */
ZTEST(zephyrdb_kv_events, test_no_event_on_backend_unavailable)
{
	zdb_kv_t kv;
	zdb_status_t rc;
	uint32_t val = 1U;
	zdb_cfg_t cfg_no_backend = g_cfg;

	cfg_no_backend.kv_backend_fs = NULL;

	rc = zdb_init(&g_db, &cfg_no_backend);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "ns", &kv);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval without backend");

	/* kv_open failed so kv handle is invalid; no set/delete should be attempted. */
	rc = zdb_kv_set(&kv, "k", &val, sizeof(val));
	zassert_not_equal(rc, ZDB_OK, "set should fail with invalid handle");
	zassert_equal(g_event_count, 0U, "no event expected when set fails pre-lock");

	zdb_deinit(&g_db);
}

/*
 * A failed invalid-arg check before backend work must not emit an event.
 */
ZTEST(zephyrdb_kv_events, test_no_event_on_inval_set)
{
	zdb_status_t rc;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_set(NULL, "k", "v", 1U);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval");
	zassert_equal(g_event_count, 0U, "no event expected on inval");

	zdb_deinit(&g_db);
}

/*
 * After a successful set, the event must be delivered to all valid listeners
 * with the real success status.  Null-notify slots are skipped without
 * crashing.
 */
ZTEST(zephyrdb_kv_events, test_set_emits_event_with_correct_fields)
{
	zdb_kv_t kv;
	uint32_t val = 99U;
	zdb_status_t rc;
	zdb_cfg_t cfg = g_cfg;

	cfg.kv_backend_fs = &g_zms;

	rc = zdb_init(&g_db, &cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "myns", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open failed: %d", rc);

	rc = zdb_kv_set(&kv, "mykey", &val, sizeof(val));
	zassert_equal(rc, ZDB_OK, "kv_set failed: %d", rc);

	zassert_equal(g_event_count, 1U, "expected 1 event, got %u", g_event_count);
	zassert_equal(g_last_event.type, ZDB_EVENT_KV_SET,
		      "wrong type: %d", g_last_event.type);
	zassert_equal(g_last_event.status, ZDB_OK,
		      "wrong status: %d", g_last_event.status);
	zassert_equal(g_last_event.value_len, sizeof(val),
		      "wrong value_len: %zu", g_last_event.value_len);
	zassert_equal(strcmp(g_last_event.namespace_name, "myns"), 0,
		      "wrong namespace");
	zassert_equal(strcmp(g_last_event.key, "mykey"), 0, "wrong key");

	/* Both real listeners fired; null-notify slot was skipped. */
	zassert_equal(g_second_count, 1U, "second listener expected 1 call");

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}

/*
 * After a delete, the event must carry ZDB_EVENT_KV_DELETE, the real
 * success status, and value_len == 0.
 */
ZTEST(zephyrdb_kv_events, test_delete_emits_event_with_correct_fields)
{
	zdb_kv_t kv;
	uint32_t val = 7U;
	zdb_status_t rc;
	zdb_cfg_t cfg = g_cfg;

	cfg.kv_backend_fs = &g_zms;

	rc = zdb_init(&g_db, &cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "delns", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open failed: %d", rc);

	rc = zdb_kv_set(&kv, "victim", &val, sizeof(val));
	zassert_equal(rc, ZDB_OK, "kv_set failed: %d", rc);
	zassert_equal(g_event_count, 1U, "set should emit one event");

	rc = zdb_kv_delete(&kv, "victim");
	zassert_equal(rc, ZDB_OK, "kv_delete failed: %d", rc);

	zassert_equal(g_event_count, 2U, "expected 2 events, got %u", g_event_count);
	zassert_equal(g_last_event.type, ZDB_EVENT_KV_DELETE,
		      "wrong type: %d", g_last_event.type);
	zassert_equal(g_last_event.status, ZDB_OK,
		      "wrong status: %d", g_last_event.status);
	zassert_equal(g_last_event.value_len, 0U,
		      "delete event value_len must be 0, got %zu", g_last_event.value_len);
	zassert_equal(strcmp(g_last_event.namespace_name, "delns"), 0,
		      "wrong namespace");
	zassert_equal(strcmp(g_last_event.key, "victim"), 0, "wrong key");

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}

/*
 * Zero-length listener list: no crash, no events, operations unaffected.
 */
ZTEST(zephyrdb_kv_events, test_empty_listener_list_no_crash)
{
	zdb_cfg_t cfg_empty = g_cfg;
	zdb_kv_t kv;
	zdb_status_t rc;
	uint32_t val = 5U;

	cfg_empty.event_listeners = NULL;
	cfg_empty.event_listener_count = 0U;
	cfg_empty.kv_backend_fs = &g_zms;

	rc = zdb_init(&g_db, &cfg_empty);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "ns", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open failed: %d", rc);

	rc = zdb_kv_set(&kv, "k", &val, sizeof(val));
	zassert_equal(rc, ZDB_OK, "kv_set failed: %d", rc);

	zassert_equal(g_event_count, 0U, "no event expected with empty listener list");

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}

/*
 * Past CONFIG_ZDB_KV_INDEX_MAX_ENTRIES a key is still written and still
 * readable by name, but nothing can enumerate it. That used to be completely
 * silent, which made a full index look identical to a working one.
 */
ZTEST(zephyrdb_kv_events, test_index_full_is_reported)
{
	zdb_kv_t kv;
	zdb_status_t rc;
	char key[16];
	uint32_t val = 7U;
	uint32_t i;

	g_cfg.kv_backend_fs = &g_zms;
	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "fullns", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open failed: %d", rc);

	/*
	 * Write past the bound rather than assuming how many entries earlier
	 * tests left behind — the index is persisted and shared by the backend.
	 */
	for (i = 0U; i < (CONFIG_ZDB_KV_INDEX_MAX_ENTRIES + 8U); i++) {
		(void)snprintk(key, sizeof(key), "k%u", i);
		rc = zdb_kv_set(&kv, key, &val, sizeof(val));
		zassert_equal(rc, ZDB_OK, "set %s failed: %d", key, rc);

		if (g_index_full_count > 0U) {
			break;
		}
	}

	zassert_true(g_index_full_count > 0U,
		     "filled the index past %d entries without reporting it",
		     CONFIG_ZDB_KV_INDEX_MAX_ENTRIES);
	zassert_equal(g_last_index_full.type, ZDB_EVENT_KV_INDEX_FULL, "wrong event type");
	zassert_equal(g_last_index_full.status, ZDB_OK,
		      "the value was stored, so the event reports success");
	zassert_equal(strcmp(g_last_index_full.namespace_name, "fullns"), 0, "wrong namespace");

	/* The value that triggered it must still be readable by name. */
	{
		uint32_t read_back = 0U;
		size_t out_len = 0U;

		rc = zdb_kv_get(&kv, g_last_index_full.key, &read_back, sizeof(read_back),
				&out_len);
		zassert_equal(rc, ZDB_OK, "unindexed key is not readable: %d", rc);
		zassert_equal(read_back, val, "unindexed key read back wrong");
	}

	/*
	 * The index is persisted and shared by the backend, so leaving it full
	 * would make every later test run against a saturated index — and see
	 * an extra INDEX_FULL event on each set. Give the capacity back.
	 */
	for (uint32_t k = 0U; k <= i; k++) {
		(void)snprintk(key, sizeof(key), "k%u", k);
		(void)zdb_kv_delete(&kv, key);
	}

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}

ZTEST_SUITE(zephyrdb_kv_events, NULL, NULL, setup, NULL, NULL);
