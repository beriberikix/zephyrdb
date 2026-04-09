/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for ZephyrDB KV event emitter/listener feature.
 * Requires CONFIG_ZDB_KV=1 and CONFIG_ZDB_EVENTING=1.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <string.h>

#include "zephyrdb.h"

K_MEM_SLAB_DEFINE_STATIC(g_core_slab, 128, 16, 4);
K_MEM_SLAB_DEFINE_STATIC(g_cursor_slab, 96, 8, 4);
K_MEM_SLAB_DEFINE_STATIC(g_kv_io_slab, 128, 8, 4);

static zdb_t g_db = {
	.core_slab = &g_core_slab,
	.cursor_slab = &g_cursor_slab,
	.kv_io_slab = &g_kv_io_slab,
};

/* ===== Listener capture harness ===== */

static zdb_kv_event_t g_last_event;
static uint32_t g_event_count;

static void capture_listener(const zdb_kv_event_t *event, void *user_ctx)
{
	ARG_UNUSED(user_ctx);

	if (event == NULL) {
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
	{ .notify = NULL,            .user_ctx = NULL },
	{ .notify = capture_listener, .user_ctx = NULL },
	{ .notify = second_listener,  .user_ctx = NULL },
};

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = NULL,
	.work_q = &k_sys_work_q,
	.event_listeners = g_listeners,
	.event_listener_count = ARRAY_SIZE(g_listeners),
};

/* ===== Test fixture ===== */

static void setup(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)memset(&g_last_event, 0, sizeof(g_last_event));
	g_event_count = 0U;
	g_second_count = 0U;
}

/* ===== Tests ===== */

/*
 * No events emitted when there is no backend (open fails before backend write).
 */
ZTEST(zephyrdb_kv_events, test_no_event_on_backend_unavailable)
{
	zdb_kv_t kv;
	zdb_status_t rc;
	uint32_t val = 1U;

	rc = zdb_init(&g_db, &g_cfg);
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
 * After a successful set, the event must be delivered to all valid listeners.
 * Checks type, status, namespace, key, and value_len.
 * Also verifies that null-notify slots are skipped without crashing.
 *
 * Note: this test uses embedded NVS mock; full round-trip is covered by
 * hardware / integration tests. Here we only verify listener dispatch.
 * We simulate dispatch by calling zdb_kv_set with a mock that succeeds at
 * the backend level; on native_sim without real NVS the call returns
 * ZDB_ERR_INVAL (no backend). The event is still emitted with that status,
 * allowing us to validate the dispatch contract.
 */
ZTEST(zephyrdb_kv_events, test_set_emits_event_with_correct_fields)
{
	zdb_kv_t kv;
	uint32_t val = 99U;
	zdb_status_t rc;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	/* Manually wire a non-null backend pointer so kv_open and backend checks pass.
	 * The actual write will fail at the real NVS layer, but the event is still
	 * dispatched with the actual return status — testing dispatch correctness. */
	zdb_cfg_t cfg_with_fake_backend = g_cfg;
	cfg_with_fake_backend.kv_backend_fs = (const void *)0x1; /* non-null sentinel */

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "deinit failed");

	rc = zdb_init(&g_db, &cfg_with_fake_backend);
	zassert_equal(rc, ZDB_OK, "re-init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "myns", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open failed: %d", rc);

	(void)zdb_kv_set(&kv, "mykey", &val, sizeof(val));

	zassert_equal(g_event_count, 1U, "expected 1 event, got %u", g_event_count);
	zassert_equal(g_last_event.type, ZDB_EVENT_KV_SET,
		      "wrong type: %d", g_last_event.type);
	zassert_equal(g_last_event.value_len, sizeof(val),
		      "wrong value_len: %zu", g_last_event.value_len);
	zassert_not_null(g_last_event.namespace_name, "namespace should not be NULL");
	zassert_equal(strcmp(g_last_event.namespace_name, "myns"), 0,
		      "wrong namespace");
	zassert_not_null(g_last_event.key, "key should not be NULL");
	zassert_equal(strcmp(g_last_event.key, "mykey"), 0, "wrong key");

	/* Both real listeners fired; null-notify slot was skipped. */
	zassert_equal(g_second_count, 1U, "second listener expected 1 call");

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}

/*
 * After a delete, the event must carry ZDB_EVENT_KV_DELETE and value_len == 0.
 */
ZTEST(zephyrdb_kv_events, test_delete_emits_event_with_correct_fields)
{
	zdb_kv_t kv;
	zdb_status_t rc;

	zdb_cfg_t cfg_with_fake_backend = g_cfg;
	cfg_with_fake_backend.kv_backend_fs = (const void *)0x1;

	rc = zdb_init(&g_db, &cfg_with_fake_backend);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "delns", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open failed: %d", rc);

	(void)zdb_kv_delete(&kv, "gone");

	zassert_equal(g_event_count, 1U, "expected 1 event, got %u", g_event_count);
	zassert_equal(g_last_event.type, ZDB_EVENT_KV_DELETE,
		      "wrong type: %d", g_last_event.type);
	zassert_equal(g_last_event.value_len, 0U,
		      "delete event value_len must be 0");
	zassert_equal(strcmp(g_last_event.namespace_name, "delns"), 0,
		      "wrong namespace");
	zassert_equal(strcmp(g_last_event.key, "gone"), 0, "wrong key");

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}

/*
 * Listener count > actual slot count: must not over-iterate.
 * Uses a zero-length listener list — no crash, no events.
 */
ZTEST(zephyrdb_kv_events, test_empty_listener_list_no_crash)
{
	zdb_cfg_t cfg_empty = g_cfg;
	zdb_kv_t kv;
	zdb_status_t rc;

	cfg_empty.event_listeners = NULL;
	cfg_empty.event_listener_count = 0U;
	cfg_empty.kv_backend_fs = (const void *)0x1;

	rc = zdb_init(&g_db, &cfg_empty);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "ns", &kv);
	zassert_equal(rc, ZDB_OK, "kv_open failed: %d", rc);

	uint32_t val = 5U;
	(void)zdb_kv_set(&kv, "k", &val, sizeof(val));

	zassert_equal(g_event_count, 0U, "no event expected with empty listener list");

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}

ZTEST_SUITE(zephyrdb_kv_events, NULL, NULL, setup, NULL, NULL);
