/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * DOC unit tests against the real LittleFS backend, using the fstab
 * automounted /lfs filesystem provided by boards/native_sim.overlay.
 *
 * zdb_doc_query() scans every collection, so each test uses unique
 * collection and field names to stay isolated from its neighbours.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include "zephyrdb.h"

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void doc_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void doc_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(doc_suite, NULL, NULL, doc_before, doc_after, NULL);

ZTEST(doc_suite, test_doc_create_close)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_create", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	rc = zdb_doc_close(&doc);
	zassert_equal(rc, ZDB_OK, "close failed: %d", rc);
}

ZTEST(doc_suite, test_doc_rejects_path_traversal_names)
{
	zdb_doc_t doc;

	zassert_equal(zdb_doc_create(&g_db, "..", "ok", &doc), ZDB_ERR_INVAL);
	zassert_equal(zdb_doc_create(&g_db, "users/evil", "ok", &doc), ZDB_ERR_INVAL);
	zassert_equal(zdb_doc_create(&g_db, "users", "../escape", &doc), ZDB_ERR_INVAL);
	zassert_equal(zdb_doc_create(&g_db, "users", "bad\\name", &doc), ZDB_ERR_INVAL);
	zassert_equal(zdb_doc_create(&g_db, "users", NULL, &doc), ZDB_ERR_INVAL);
	zassert_equal(zdb_doc_create(&g_db, NULL, "ok", &doc), ZDB_ERR_INVAL);
}

ZTEST(doc_suite, test_doc_field_set_all_types)
{
	zdb_doc_t doc;
	const uint8_t bytes_val[] = {0xDE, 0xAD, 0xBE, 0xEF};
	zdb_status_t rc = zdb_doc_create(&g_db, "c_types", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);

	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 30), ZDB_OK);
	zassert_equal(zdb_doc_field_set_f64(&doc, "rating", 4.5), ZDB_OK);
	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Alice"), ZDB_OK);
	zassert_equal(zdb_doc_field_set_bool(&doc, "active", true), ZDB_OK);
	zassert_equal(zdb_doc_field_set_bytes(&doc, "data", bytes_val, sizeof(bytes_val)),
		      ZDB_OK);

	rc = zdb_doc_close(&doc);
	zassert_equal(rc, ZDB_OK, "close failed: %d", rc);
}

ZTEST(doc_suite, test_doc_field_get_roundtrip)
{
	zdb_doc_t doc;
	int64_t age = 0;
	double rating = 0.0;
	const char *name = NULL;
	bool active = false;
	zdb_bytes_t data = {0};
	const uint8_t bytes_val[] = {0x01, 0x02, 0x03};
	zdb_status_t rc = zdb_doc_create(&g_db, "c_rtrip", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);

	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 30), ZDB_OK);
	zassert_equal(zdb_doc_field_set_f64(&doc, "rating", 4.5), ZDB_OK);
	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Alice"), ZDB_OK);
	zassert_equal(zdb_doc_field_set_bool(&doc, "active", true), ZDB_OK);
	zassert_equal(zdb_doc_field_set_bytes(&doc, "data", bytes_val, sizeof(bytes_val)),
		      ZDB_OK);

	zassert_equal(zdb_doc_field_get_i64(&doc, "age", &age), ZDB_OK);
	zassert_equal(age, 30, "age mismatch: %lld", (long long)age);

	zassert_equal(zdb_doc_field_get_f64(&doc, "rating", &rating), ZDB_OK);
	zassert_within(rating, 4.5, 0.0001, "rating mismatch");

	zassert_equal(zdb_doc_field_get_string(&doc, "name", &name), ZDB_OK);
	zassert_str_equal(name, "Alice", "name mismatch");

	zassert_equal(zdb_doc_field_get_bool(&doc, "active", &active), ZDB_OK);
	zassert_true(active, "active flag mismatch");

	zassert_equal(zdb_doc_field_get_bytes(&doc, "data", &data), ZDB_OK);
	zassert_equal(data.len, sizeof(bytes_val), "bytes length mismatch");
	zassert_mem_equal(data.data, bytes_val, sizeof(bytes_val), "bytes mismatch");

	zdb_doc_close(&doc);
}

ZTEST(doc_suite, test_doc_field_get_wrong_type)
{
	zdb_doc_t doc;
	const char *result = NULL;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_wrong", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 30), ZDB_OK);

	rc = zdb_doc_field_get_string(&doc, "age", &result);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected INVAL for type mismatch, got %d", rc);

	zdb_doc_close(&doc);
}

ZTEST(doc_suite, test_doc_field_get_missing)
{
	zdb_doc_t doc;
	int64_t out = 0;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_miss", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);

	rc = zdb_doc_field_get_i64(&doc, "never_set", &out);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "expected NOT_FOUND, got %d", rc);

	zdb_doc_close(&doc);
}

ZTEST(doc_suite, test_doc_field_overwrite)
{
	zdb_doc_t doc;
	int64_t retrieved = 0;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_ow", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 25), ZDB_OK);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 35), ZDB_OK);

	zassert_equal(zdb_doc_field_get_i64(&doc, "age", &retrieved), ZDB_OK);
	zassert_equal(retrieved, 35, "value not overwritten: %lld", (long long)retrieved);

	zdb_doc_close(&doc);
}

ZTEST(doc_suite, test_doc_save_open_roundtrip)
{
	zdb_doc_t doc;
	zdb_doc_t reopened;
	const char *name = NULL;
	int64_t age = 0;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_persist", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Charlie"), ZDB_OK);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 40), ZDB_OK);

	rc = zdb_doc_save(&doc);
	zassert_equal(rc, ZDB_OK, "save failed: %d", rc);
	rc = zdb_doc_close(&doc);
	zassert_equal(rc, ZDB_OK, "close failed: %d", rc);

	rc = zdb_doc_open(&g_db, "c_persist", "d1", &reopened);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	zassert_equal(zdb_doc_field_get_string(&reopened, "name", &name), ZDB_OK);
	zassert_str_equal(name, "Charlie", "name not persisted");
	zassert_equal(zdb_doc_field_get_i64(&reopened, "age", &age), ZDB_OK);
	zassert_equal(age, 40, "age not persisted: %lld", (long long)age);

	zdb_doc_close(&reopened);
}

ZTEST(doc_suite, test_doc_delete)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_del", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "x", 1), ZDB_OK);
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");
	zassert_equal(zdb_doc_close(&doc), ZDB_OK, "close failed");

	rc = zdb_doc_delete(&g_db, "c_del", "d1");
	zassert_equal(rc, ZDB_OK, "delete failed: %d", rc);

	rc = zdb_doc_open(&g_db, "c_del", "d1", &doc);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "document still exists after delete: %d", rc);
}

ZTEST(doc_suite, test_doc_query_filters)
{
	const struct {
		const char *doc_id;
		const char *name;
		int64_t age;
		bool active;
	} docs[] = {
		{"user_a", "Alice", 30, true},
		{"user_b", "Bob", 25, false},
		{"user_c", "Charlie", 30, true},
	};
	zdb_doc_query_filter_t age_filter = {
		.field_name = "qf_age",
		.type = ZDB_DOC_FIELD_INT64,
		.numeric_value = 30.0,
	};
	zdb_doc_query_filter_t combo_filters[2];
	zdb_doc_query_t query = {0};
	zdb_doc_metadata_t results[8];
	size_t count;
	zdb_status_t rc;

	for (size_t i = 0U; i < ARRAY_SIZE(docs); i++) {
		zdb_doc_t doc;

		rc = zdb_doc_create(&g_db, "c_query", docs[i].doc_id, &doc);
		zassert_equal(rc, ZDB_OK, "create %zu failed: %d", i, rc);
		zassert_equal(zdb_doc_field_set_string(&doc, "qf_name", docs[i].name), ZDB_OK);
		zassert_equal(zdb_doc_field_set_i64(&doc, "qf_age", docs[i].age), ZDB_OK);
		zassert_equal(zdb_doc_field_set_bool(&doc, "qf_active", docs[i].active), ZDB_OK);
		zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save %zu failed", i);
		zassert_equal(zdb_doc_close(&doc), ZDB_OK, "close %zu failed", i);
	}

	/* Count-only mode: NULL output array returns the match count. */
	query.filters = &age_filter;
	query.filter_count = 1U;
	count = 0U;
	rc = zdb_doc_query(&g_db, &query, NULL, &count);
	zassert_equal(rc, ZDB_OK, "count-only query failed: %d", rc);
	zassert_equal(count, 2U, "expected 2 matches, got %zu", count);

	/* Materialized query returns metadata for each match. */
	count = ARRAY_SIZE(results);
	rc = zdb_doc_query(&g_db, &query, results, &count);
	zassert_equal(rc, ZDB_OK, "query failed: %d", rc);
	zassert_equal(count, 2U, "expected 2 results, got %zu", count);
	for (size_t i = 0U; i < count; i++) {
		zassert_str_equal(results[i].collection_name, "c_query",
				  "unexpected collection");
		zassert_equal(results[i].field_count, 3U, "unexpected field count");
	}
	zassert_equal(zdb_doc_metadata_free(results, count), ZDB_OK);

	/* AND-combined filters narrow the result. */
	combo_filters[0] = age_filter;
	combo_filters[1] = (zdb_doc_query_filter_t){
		.field_name = "qf_name",
		.type = ZDB_DOC_FIELD_STRING,
		.string_value = "Alice",
	};
	query.filters = combo_filters;
	query.filter_count = 2U;
	count = 0U;
	rc = zdb_doc_query(&g_db, &query, NULL, &count);
	zassert_equal(rc, ZDB_OK, "combo query failed: %d", rc);
	zassert_equal(count, 1U, "expected 1 combo match, got %zu", count);

	/* Limit caps the scan. */
	query.filters = &age_filter;
	query.filter_count = 1U;
	query.limit = 1U;
	count = 0U;
	rc = zdb_doc_query(&g_db, &query, NULL, &count);
	zassert_equal(rc, ZDB_OK, "limited query failed: %d", rc);
	zassert_equal(count, 1U, "limit not honoured, got %zu", count);
}

ZTEST(doc_suite, test_doc_export_flatbuffer_unsupported)
{
	zdb_doc_t doc;
	uint8_t buffer[256];
	size_t out_len = 0U;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_fb", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "x", 1), ZDB_OK);

	/* Documented stub: FlatBuffers document export is not implemented. */
	rc = zdb_doc_export_flatbuffer(&doc, buffer, sizeof(buffer), &out_len);
	zassert_equal(rc, ZDB_ERR_UNSUPPORTED, "expected UNSUPPORTED stub, got %d", rc);

	zdb_doc_close(&doc);
}

ZTEST(doc_suite, test_doc_open_fails_on_header_crc_corruption)
{
	zdb_doc_t doc;
	zdb_doc_t reopened;
	struct fs_file_t file;
	zdb_status_t rc;
	int fs_rc;

	struct {
		uint32_t magic_le;
		uint16_t version_le;
		uint16_t field_count_le;
		uint64_t created_ms_le;
		uint64_t updated_ms_le;
		uint32_t crc_le;
	} __packed hdr;

	rc = zdb_doc_create(&g_db, "c_crc", "d1", &doc);
	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 33), ZDB_OK);
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");
	zassert_equal(zdb_doc_close(&doc), ZDB_OK, "close failed");

	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/zdb_docs/c_crc/d1.zdoc", FS_O_READ | FS_O_WRITE);
	zassert_equal(fs_rc, 0, "opening saved doc file failed: %d", fs_rc);

	fs_rc = fs_read(&file, &hdr, sizeof(hdr));
	zassert_equal(fs_rc, (int)sizeof(hdr), "reading doc header failed: %d", fs_rc);

	hdr.crc_le ^= 0x1U;

	fs_rc = fs_seek(&file, 0, FS_SEEK_SET);
	zassert_equal(fs_rc, 0, "seek failed: %d", fs_rc);
	fs_rc = fs_write(&file, &hdr, sizeof(hdr));
	zassert_equal(fs_rc, (int)sizeof(hdr), "writing corrupted header failed: %d", fs_rc);
	fs_rc = fs_close(&file);
	zassert_equal(fs_rc, 0, "closing doc file failed: %d", fs_rc);

	rc = zdb_doc_open(&g_db, "c_crc", "d1", &reopened);
	zassert_equal(rc, ZDB_ERR_CORRUPT, "expected CORRUPT on bad header CRC, got %d", rc);
}
