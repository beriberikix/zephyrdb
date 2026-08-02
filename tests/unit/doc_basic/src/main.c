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
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
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

#if defined(CONFIG_ZDB_FLATBUFFERS) && (CONFIG_ZDB_FLATBUFFERS)
/*
 * FlatBuffers export. These need the flatcc runtime, which CI's workspace does
 * not fetch, so they are compiled out unless the build enables it.
 */
ZTEST(doc_suite, test_doc_export_flatbuffer_roundtrip)
{
	zdb_doc_t doc;
	uint8_t buffer[512];
	const uint8_t blob[] = {0xDE, 0xAD, 0xBE, 0xEF};
	size_t out_len = 0U;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_fb", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 30), ZDB_OK);
	zassert_equal(zdb_doc_field_set_f64(&doc, "rating", 4.5), ZDB_OK);
	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Alice"), ZDB_OK);
	zassert_equal(zdb_doc_field_set_bool(&doc, "active", true), ZDB_OK);
	zassert_equal(zdb_doc_field_set_bytes(&doc, "data", blob, sizeof(blob)), ZDB_OK);

	rc = zdb_doc_export_flatbuffer(&doc, buffer, sizeof(buffer), &out_len);
	zassert_equal(rc, ZDB_OK, "export failed: %d", rc);
	zassert_true(out_len > 0U, "export produced no bytes");
	zassert_true(out_len <= sizeof(buffer), "export overran the buffer: %zu", out_len);

	/* The payload should carry the strings we put in it. */
	{
		bool saw_name = false;

		for (size_t i = 0U; (i + 5U) <= out_len; i++) {
			if (memcmp(&buffer[i], "Alice", 5U) == 0) {
				saw_name = true;
				break;
			}
		}
		zassert_true(saw_name, "string field missing from the exported buffer");
	}

	zdb_doc_close(&doc);
}

/* A NULL buffer asks how much space the export needs. */
ZTEST(doc_suite, test_doc_export_flatbuffer_size_query)
{
	zdb_doc_t doc;
	uint8_t buffer[512];
	size_t query_len = 0U;
	size_t actual_len = 0U;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_fb_size", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "x", 7), ZDB_OK);

	rc = zdb_doc_export_flatbuffer(&doc, NULL, 0U, &query_len);
	zassert_equal(rc, ZDB_OK, "size query failed: %d", rc);
	zassert_true(query_len > 0U, "size query reported zero");

	rc = zdb_doc_export_flatbuffer(&doc, buffer, sizeof(buffer), &actual_len);
	zassert_equal(rc, ZDB_OK, "export failed: %d", rc);
	zassert_equal(actual_len, query_len, "size query disagreed with the export: %zu vs %zu",
		      query_len, actual_len);

	zdb_doc_close(&doc);
}

/* A short buffer must report the requirement rather than overrun. */
ZTEST(doc_suite, test_doc_export_flatbuffer_buffer_too_small)
{
	zdb_doc_t doc;
	uint8_t small[8];
	size_t needed = 0U;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_fb_small", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_string(&doc, "name", "a longer value"), ZDB_OK);

	rc = zdb_doc_export_flatbuffer(&doc, small, sizeof(small), &needed);
	zassert_equal(rc, ZDB_ERR_NOMEM, "expected NOMEM for a short buffer, got %d", rc);
	zassert_true(needed > sizeof(small), "required size not reported: %zu", needed);

	zdb_doc_close(&doc);
}
#else
/* Without the flatcc runtime the export reports that it is unavailable. */
ZTEST(doc_suite, test_doc_export_flatbuffer_unsupported)
{
	zdb_doc_t doc;
	uint8_t buffer[256];
	size_t out_len = 0U;
	zdb_status_t rc = zdb_doc_create(&g_db, "c_fb", "d1", &doc);

	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "x", 1), ZDB_OK);

	rc = zdb_doc_export_flatbuffer(&doc, buffer, sizeof(buffer), &out_len);
	zassert_equal(rc, ZDB_ERR_UNSUPPORTED, "expected UNSUPPORTED, got %d", rc);

	zdb_doc_close(&doc);
}
#endif /* CONFIG_ZDB_FLATBUFFERS */

/*
 * The v2 CRC covers field payloads, so a flipped payload byte must be caught
 * on open instead of surfacing as a plausible-looking value.
 */
ZTEST(doc_suite, test_doc_open_fails_on_payload_corruption)
{
	zdb_doc_t doc;
	zdb_doc_t reopened;
	struct fs_file_t file;
	zdb_status_t rc;
	int fs_rc;
	uint8_t byte;
	off_t payload_off;

	rc = zdb_doc_create(&g_db, "c_payload", "d1", &doc);
	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 33), ZDB_OK);
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");
	zassert_equal(zdb_doc_close(&doc), ZDB_OK, "close failed");

	/* 28-byte header + 8-byte field header + 3-byte name lands on the value. */
	payload_off = 28 + 8 + 3;

	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/zdb_docs/c_payload/d1.zdoc", FS_O_READ | FS_O_WRITE);
	zassert_equal(fs_rc, 0, "opening saved doc file failed: %d", fs_rc);

	fs_rc = fs_seek(&file, payload_off, FS_SEEK_SET);
	zassert_equal(fs_rc, 0, "seek failed: %d", fs_rc);
	fs_rc = fs_read(&file, &byte, sizeof(byte));
	zassert_equal(fs_rc, (int)sizeof(byte), "reading payload byte failed: %d", fs_rc);

	byte ^= 0xFFU;

	fs_rc = fs_seek(&file, payload_off, FS_SEEK_SET);
	zassert_equal(fs_rc, 0, "seek failed: %d", fs_rc);
	fs_rc = fs_write(&file, &byte, sizeof(byte));
	zassert_equal(fs_rc, (int)sizeof(byte), "writing corrupted payload failed: %d", fs_rc);
	fs_rc = fs_close(&file);
	zassert_equal(fs_rc, 0, "closing doc file failed: %d", fs_rc);

	rc = zdb_doc_open(&g_db, "c_payload", "d1", &reopened);
	zassert_equal(rc, ZDB_ERR_CORRUPT, "expected CORRUPT on payload flip, got %d", rc);
}

/* A save interrupted mid-payload leaves a short file; opening must not guess. */
ZTEST(doc_suite, test_doc_open_fails_on_truncated_payload)
{
	zdb_doc_t doc;
	zdb_doc_t reopened;
	struct fs_file_t file;
	zdb_status_t rc;
	int fs_rc;

	rc = zdb_doc_create(&g_db, "c_trunc", "d1", &doc);
	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Charlie"), ZDB_OK);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 40), ZDB_OK);
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");
	zassert_equal(zdb_doc_close(&doc), ZDB_OK, "close failed");

	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/zdb_docs/c_trunc/d1.zdoc", FS_O_READ | FS_O_WRITE);
	zassert_equal(fs_rc, 0, "opening saved doc file failed: %d", fs_rc);
	/* Keep the header and the first field header, drop the rest. */
	fs_rc = fs_truncate(&file, 28 + 8 + 2);
	zassert_equal(fs_rc, 0, "truncate failed: %d", fs_rc);
	fs_rc = fs_close(&file);
	zassert_equal(fs_rc, 0, "closing doc file failed: %d", fs_rc);

	rc = zdb_doc_open(&g_db, "c_trunc", "d1", &reopened);
	zassert_equal(rc, ZDB_ERR_CORRUPT, "expected CORRUPT on truncated file, got %d", rc);
}

/*
 * Crash window: the staging file was written and renamed away from the live
 * name. Opening promotes it rather than reporting the document missing.
 */
ZTEST(doc_suite, test_doc_open_recovers_staged_save)
{
	zdb_doc_t doc;
	zdb_doc_t reopened;
	int64_t age = 0;
	zdb_status_t rc;
	int fs_rc;

	rc = zdb_doc_create(&g_db, "c_stage", "d1", &doc);
	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 77), ZDB_OK);
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");
	zassert_equal(zdb_doc_close(&doc), ZDB_OK, "close failed");

	/* Simulate a crash between staging and rename. */
	fs_rc = fs_rename("/lfs/zdb_docs/c_stage/d1.zdoc", "/lfs/zdb_docs/c_stage/d1.zdoc.tmp");
	zassert_equal(fs_rc, 0, "staging rename failed: %d", fs_rc);

	rc = zdb_doc_open(&g_db, "c_stage", "d1", &reopened);
	zassert_equal(rc, ZDB_OK, "staged save not recovered: %d", rc);
	zassert_equal(zdb_doc_field_get_i64(&reopened, "age", &age), ZDB_OK);
	zassert_equal(age, 77, "recovered wrong value: %lld", (long long)age);
	zdb_doc_close(&reopened);

	/* The staging file was promoted, so a second open needs no recovery. */
	rc = zdb_doc_open(&g_db, "c_stage", "d1", &reopened);
	zassert_equal(rc, ZDB_OK, "promoted document not readable: %d", rc);
	zdb_doc_close(&reopened);
}

/* An unparsable staging file must not shadow or resurrect anything. */
ZTEST(doc_suite, test_doc_save_discards_stale_staging_file)
{
	zdb_doc_t doc;
	zdb_doc_t reopened;
	struct fs_file_t file;
	const uint8_t junk[16] = {0xFFU};
	int64_t age = 0;
	zdb_status_t rc;
	int fs_rc;

	rc = zdb_doc_create(&g_db, "c_stale", "d1", &doc);
	zassert_equal(rc, ZDB_OK, "create failed: %d", rc);
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 1), ZDB_OK);
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "initial save failed");

	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/zdb_docs/c_stale/d1.zdoc.tmp",
			FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	zassert_equal(fs_rc, 0, "creating stale staging file failed: %d", fs_rc);
	fs_rc = fs_write(&file, junk, sizeof(junk));
	zassert_equal(fs_rc, (int)sizeof(junk), "writing junk failed: %d", fs_rc);
	zassert_equal(fs_close(&file), 0, "closing staging file failed");

	/* A save overwrites the staging file and renames it into place. */
	zassert_equal(zdb_doc_field_set_i64(&doc, "age", 2), ZDB_OK);
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save over stale staging file failed");
	zassert_equal(zdb_doc_close(&doc), ZDB_OK, "close failed");

	rc = zdb_doc_open(&g_db, "c_stale", "d1", &reopened);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);
	zassert_equal(zdb_doc_field_get_i64(&reopened, "age", &age), ZDB_OK);
	zassert_equal(age, 2, "stale staging file won: %lld", (long long)age);
	zdb_doc_close(&reopened);

	/* The staging file is gone, so open finds nothing to recover after delete. */
	zassert_equal(zdb_doc_delete(&g_db, "c_stale", "d1"), ZDB_OK, "delete failed");
	rc = zdb_doc_open(&g_db, "c_stale", "d1", &reopened);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "document resurrected from staging file: %d", rc);
}

/* v1 files predate the payload CRC and must still open. */
ZTEST(doc_suite, test_doc_open_accepts_v1_format)
{
	zdb_doc_t reopened;
	struct fs_file_t file;
	zdb_status_t rc;
	int fs_rc;
	int64_t age = 0;

	struct {
		uint32_t magic_le;
		uint16_t version_le;
		uint16_t field_count_le;
		uint64_t created_ms_le;
		uint64_t updated_ms_le;
		uint32_t crc_le;
	} __packed hdr;

	struct {
		uint16_t name_len_le;
		uint16_t reserved_le;
		uint8_t type;
		uint8_t reserved[3];
	} __packed fh;

	uint64_t value_le = sys_cpu_to_le64((uint64_t)42);

	/* Create the collection directory via a normal save, then hand-write v1. */
	{
		zdb_doc_t seed;

		zassert_equal(zdb_doc_create(&g_db, "c_v1", "seed", &seed), ZDB_OK);
		zassert_equal(zdb_doc_field_set_i64(&seed, "x", 0), ZDB_OK);
		zassert_equal(zdb_doc_save(&seed), ZDB_OK, "seed save failed");
		zassert_equal(zdb_doc_close(&seed), ZDB_OK);
	}

	hdr.magic_le = sys_cpu_to_le32(0x5A444F43u);
	hdr.version_le = sys_cpu_to_le16(1U);
	hdr.field_count_le = sys_cpu_to_le16(1U);
	hdr.created_ms_le = sys_cpu_to_le64(1000U);
	hdr.updated_ms_le = sys_cpu_to_le64(2000U);
	/* crc_le is the last member of the packed header, so the prefix is
	 * everything before it.
	 */
	hdr.crc_le = sys_cpu_to_le32(
		crc32_ieee((const uint8_t *)&hdr, sizeof(hdr) - sizeof(hdr.crc_le)));

	fh.name_len_le = sys_cpu_to_le16(3U);
	fh.reserved_le = 0U;
	fh.type = (uint8_t)ZDB_DOC_FIELD_INT64;
	fh.reserved[0] = fh.reserved[1] = fh.reserved[2] = 0U;

	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/zdb_docs/c_v1/d1.zdoc", FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	zassert_equal(fs_rc, 0, "creating v1 doc failed: %d", fs_rc);
	zassert_equal(fs_write(&file, &hdr, sizeof(hdr)), (int)sizeof(hdr), "hdr write failed");
	zassert_equal(fs_write(&file, &fh, sizeof(fh)), (int)sizeof(fh), "field hdr write failed");
	zassert_equal(fs_write(&file, "age", 3), 3, "name write failed");
	zassert_equal(fs_write(&file, &value_le, sizeof(value_le)), (int)sizeof(value_le),
		      "value write failed");
	zassert_equal(fs_close(&file), 0, "closing v1 doc failed");

	rc = zdb_doc_open(&g_db, "c_v1", "d1", &reopened);
	zassert_equal(rc, ZDB_OK, "v1 document not readable: %d", rc);
	zassert_equal(zdb_doc_field_get_i64(&reopened, "age", &age), ZDB_OK);
	zassert_equal(age, 42, "v1 value wrong: %lld", (long long)age);
	zdb_doc_close(&reopened);
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
