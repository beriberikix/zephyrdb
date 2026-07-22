/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Integration tests: KV (ZMS on storage_partition) and TS/DOC (LittleFS
 * via boards/native_sim.overlay) working together on one zdb instance.
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

static void wf_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zms_backend_mount_once();
	g_cfg.kv_backend_fs = &g_zms;
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void wf_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(workflows, NULL, NULL, wf_before, wf_after, NULL);

/*
 * KV and TS modules are independent: both usable on one instance.
 */
ZTEST(workflows, test_kv_ts_independent)
{
	zdb_kv_t kv;
	zdb_ts_t ts;
	uint32_t kv_value = 123U;
	uint32_t kv_readback = 0U;
	size_t kv_len = 0U;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 567 };
	zdb_status_t rc;

	rc = zdb_kv_open(&g_db, "app_config", &kv);
	zassert_equal(rc, ZDB_OK, "kv open failed: %d", rc);
	rc = zdb_ts_open(&g_db, "wf_ind", &ts);
	zassert_equal(rc, ZDB_OK, "ts open failed: %d", rc);

	rc = zdb_kv_set(&kv, "setting_1", &kv_value, sizeof(kv_value));
	zassert_equal(rc, ZDB_OK, "kv set failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "ts append failed: %d", rc);

	rc = zdb_kv_get(&kv, "setting_1", &kv_readback, sizeof(kv_readback), &kv_len);
	zassert_equal(rc, ZDB_OK, "kv get failed: %d", rc);
	zassert_equal(kv_readback, kv_value, "kv value corrupted");

	zassert_equal(zdb_kv_close(&kv), ZDB_OK);
	zassert_equal(zdb_ts_close(&ts), ZDB_OK);
}

/*
 * The TS context pins one active stream per instance until deinit:
 * opening a second, different stream must fail with BUSY.
 */
ZTEST(workflows, test_ts_single_stream_per_instance)
{
	zdb_ts_t ts1;
	zdb_ts_t ts2;
	zdb_status_t rc;

	rc = zdb_ts_open(&g_db, "wf_one", &ts1);
	zassert_equal(rc, ZDB_OK, "first open failed: %d", rc);

	rc = zdb_ts_open(&g_db, "wf_two", &ts2);
	zassert_equal(rc, ZDB_ERR_BUSY, "second stream should be BUSY, got %d", rc);

	/* Re-opening the same stream is allowed. */
	rc = zdb_ts_open(&g_db, "wf_one", &ts2);
	zassert_equal(rc, ZDB_OK, "re-open of active stream failed: %d", rc);

	zdb_ts_close(&ts2);
	zdb_ts_close(&ts1);
}

/*
 * DOC operations work alongside KV/TS on the same instance.
 */
ZTEST(workflows, test_doc_alongside_kv_ts)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_db, "wf_docs", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "doc create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "v", 1), ZDB_OK);
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "doc save failed");
	zassert_equal(zdb_doc_close(&doc), ZDB_OK);
}

/*
 * Multiple KV namespaces plus a TS stream in one session, closed in
 * reverse order — no leaks, no crashes, deinit in teardown succeeds.
 */
ZTEST(workflows, test_init_deinit_sequence)
{
	zdb_kv_t kv1;
	zdb_kv_t kv2;
	zdb_ts_t ts1;
	uint32_t kv_val = 100U;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 200 };
	zdb_status_t rc;

	rc = zdb_kv_open(&g_db, "seq_ns1", &kv1);
	zassert_equal(rc, ZDB_OK, "kv1 open failed: %d", rc);
	rc = zdb_kv_open(&g_db, "seq_ns2", &kv2);
	zassert_equal(rc, ZDB_OK, "kv2 open failed: %d", rc);
	rc = zdb_ts_open(&g_db, "wf_seq", &ts1);
	zassert_equal(rc, ZDB_OK, "ts open failed: %d", rc);

	zassert_equal(zdb_kv_set(&kv1, "key1", &kv_val, sizeof(kv_val)), ZDB_OK);
	zassert_equal(zdb_kv_set(&kv2, "key2", &kv_val, sizeof(kv_val)), ZDB_OK);
	zassert_equal(zdb_ts_append_i64(&ts1, &sample), ZDB_OK);

	zassert_equal(zdb_ts_close(&ts1), ZDB_OK);
	zassert_equal(zdb_kv_close(&kv2), ZDB_OK);
	zassert_equal(zdb_kv_close(&kv1), ZDB_OK);
}

/*
 * Two cursors on the same stream iterate independently.
 */
ZTEST(workflows, test_independent_cursors_same_stream)
{
	zdb_ts_t ts;
	zdb_cursor_t cur1;
	zdb_cursor_t cur2;
	zdb_bytes_t rec;
	unsigned int count1 = 0U;
	unsigned int count2 = 0U;
	zdb_status_t rc;

	rc = zdb_ts_open(&g_db, "wf_cur", &ts);
	zassert_equal(rc, ZDB_OK, "ts open failed: %d", rc);

	for (int i = 0; i < 5; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = 1000U + (uint64_t)i,
			.value = 100 + i,
		};
		zassert_equal(zdb_ts_append_i64(&ts, &sample), ZDB_OK);
	}
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cur1);
	zassert_equal(rc, ZDB_OK, "cursor1 open failed: %d", rc);
	rc = zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cur2);
	zassert_equal(rc, ZDB_OK, "cursor2 open failed: %d", rc);

	/* Interleave: each cursor keeps its own position. */
	while (true) {
		zdb_status_t rc1 = zdb_cursor_next(&cur1, &rec);
		zdb_status_t rc2 = zdb_cursor_next(&cur2, &rec);

		if (rc1 == ZDB_OK) {
			count1++;
		}
		if (rc2 == ZDB_OK) {
			count2++;
		}
		if ((rc1 != ZDB_OK) && (rc2 != ZDB_OK)) {
			break;
		}
		zassert_true(count1 <= 5U && count2 <= 5U, "cursor overran");
	}

	zassert_equal(count1, 5U, "cursor1 saw %u records", count1);
	zassert_equal(count2, 5U, "cursor2 saw %u records", count2);

	zassert_equal(zdb_cursor_close(&cur2), ZDB_OK);
	zassert_equal(zdb_cursor_close(&cur1), ZDB_OK);
	zdb_ts_close(&ts);
}

/*
 * Health stays sane through normal operation.
 */
ZTEST(workflows, test_health_status)
{
	zdb_kv_t kv;
	uint32_t val = 42U;
	zdb_health_t health = zdb_health(&g_db);

	zassert_equal(health, ZDB_HEALTH_OK, "fresh instance health not OK: %d", health);

	zassert_equal(zdb_kv_open(&g_db, "health_ns", &kv), ZDB_OK);
	zassert_equal(zdb_kv_set(&kv, "key", &val, sizeof(val)), ZDB_OK);

	health = zdb_health(&g_db);
	zassert_not_equal(health, ZDB_HEALTH_FAULT, "health FAULT after normal ops");

	zdb_kv_close(&kv);
	zassert_equal(zdb_health(NULL), ZDB_HEALTH_FAULT, "NULL db should report FAULT");
}

/*
 * Stats export round-trips through validation and detects tampering.
 */
ZTEST(workflows, test_stats_export_and_validate)
{
	zdb_ts_stats_export_t export_data;
	zdb_status_t rc;

	rc = zdb_ts_stats_export(&g_db, &export_data);
	zassert_equal(rc, ZDB_OK, "stats export failed: %d", rc);
	zassert_equal(export_data.version, 1U, "unexpected export version");

	rc = zdb_ts_stats_export_validate(&export_data);
	zassert_equal(rc, ZDB_OK, "validate of fresh export failed: %d", rc);

	/* Tampering with any byte of the payload is detected. */
	export_data.version ^= 0xFFU;
	rc = zdb_ts_stats_export_validate(&export_data);
	zassert_equal(rc, ZDB_ERR_CORRUPT, "tampered version should be CORRUPT, got %d", rc);
	export_data.version ^= 0xFFU;

	export_data.corrupt_records ^= 0xFFU;
	rc = zdb_ts_stats_export_validate(&export_data);
	zassert_equal(rc, ZDB_ERR_CORRUPT, "tampered counter should be CORRUPT, got %d", rc);
	export_data.corrupt_records ^= 0xFFU;

	rc = zdb_ts_stats_export_validate(&export_data);
	zassert_equal(rc, ZDB_OK, "untampered export failed validation: %d", rc);

	zassert_equal(zdb_ts_stats_export(NULL, &export_data), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_stats_export_validate(NULL), ZDB_ERR_INVAL);
}

/*
 * KV workflow mirroring the kv_basic sample: set -> get -> delete -> gone.
 */
ZTEST(workflows, test_kv_basic_workflow)
{
	zdb_kv_t kv;
	uint32_t boot_count = 1U;
	uint32_t readback = 0U;
	size_t out_len = 0U;
	zdb_status_t rc;

	rc = zdb_kv_open(&g_db, "wf_app", &kv);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	zassert_equal(zdb_kv_set(&kv, "boot_count", &boot_count, sizeof(boot_count)), ZDB_OK);

	rc = zdb_kv_get(&kv, "boot_count", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_OK, "get failed: %d", rc);
	zassert_equal(readback, boot_count, "boot count mismatch");
	zassert_equal(out_len, sizeof(boot_count), "length mismatch");

	zassert_equal(zdb_kv_delete(&kv, "boot_count"), ZDB_OK);

	rc = zdb_kv_get(&kv, "boot_count", &readback, sizeof(readback), &out_len);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "expected NOT_FOUND after delete, got %d", rc);

	zdb_kv_close(&kv);
}

/*
 * DOC query workflow mirroring the doc_query_filters sample.
 */
ZTEST(workflows, test_doc_query_filters_workflow)
{
	const struct {
		const char *id;
		int64_t score;
		bool approved;
	} docs[] = {
		{"doc1", 85, true},
		{"doc2", 92, true},
		{"doc3", 85, false},
	};
	zdb_doc_query_filter_t filters[] = {
		{
			.field_name = "wf_score",
			.type = ZDB_DOC_FIELD_INT64,
			.numeric_value = 85.0,
		},
		{
			.field_name = "wf_approved",
			.type = ZDB_DOC_FIELD_BOOL,
			.bool_value = true,
		},
	};
	zdb_doc_query_t query = {
		.filters = filters,
		.filter_count = ARRAY_SIZE(filters),
	};
	size_t count = 0U;
	zdb_status_t rc;

	for (size_t i = 0U; i < ARRAY_SIZE(docs); i++) {
		zdb_doc_t doc;

		rc = zdb_doc_create(&g_db, "wf_items", docs[i].id, &doc);
		zassert_equal(rc, ZDB_OK, "create %zu failed: %d", i, rc);
		zassert_equal(zdb_doc_field_set_i64(&doc, "wf_score", docs[i].score), ZDB_OK);
		zassert_equal(zdb_doc_field_set_bool(&doc, "wf_approved", docs[i].approved),
			      ZDB_OK);
		zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save %zu failed", i);
		zassert_equal(zdb_doc_close(&doc), ZDB_OK);
	}

	/* score == 85 AND approved == true matches exactly doc1. */
	rc = zdb_doc_query(&g_db, &query, NULL, &count);
	zassert_equal(rc, ZDB_OK, "query failed: %d", rc);
	zassert_equal(count, 1U, "expected 1 match, got %zu", count);
}
