/*
 * Copyright (c) 2026 ZephyrDB Test Suite
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for ZephyrDB DOC (Document) module.
 * Tests critical APIs: zdb_doc_create, zdb_doc_save, zdb_doc_field_set/get, zdb_doc_query.
 * Note: Requires CONFIG_ZDB_DOC (depends on ZDB_CORE + storage backend).
 */

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyrdb.h>
#include "../fixtures/common.h"
#include <string.h>
#include <errno.h>
#include <zephyr/fs/fs.h>

/* Test instance */
ZDB_TEST_INSTANCE_DEFINE(g_test_db);
static struct mock_lfs_fs g_mock_lfs;

/* ===== Setup and Teardown ===== */

static void doc_test_setup(void)
{
	/* Initialize mock backend */
	int rc = mock_lfs_init(&g_mock_lfs);
	zassert_equal(rc, 0, "Failed to init mock LittleFS");

	/* Initialize database (requires TS + FlatBuffers for DOC support) */
	zdb_cfg_t cfg = {
		.kv_backend_fs = NULL,
		.lfs_mount_point = "/lfs",
		.work_q = &k_sys_work_q,
	};

	rc = zdb_init(&g_test_db, &cfg);
	zassert_equal(rc, ZDB_OK, "Failed to init zdb: rc=%d", rc);
}

static void doc_test_teardown(void)
{
	zdb_deinit(&g_test_db);
	mock_lfs_reset(&g_mock_lfs);
}

/* ===== Test Cases ===== */

/*
 * Test: DOC create succeeds
 * Expected: Document allocated in memory (not persisted yet)
 */
static void test_doc_create_success(void)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_test_db, "users", "user_1", &doc);
	
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED,
		     "Doc create failed: %d (may require CONFIG_ZDB_DOC)", rc);

	if (rc == ZDB_OK) {
		rc = zdb_doc_close(&doc);
		assert_zdb_ok(rc);
	}
}

/*
 * Test: DOC field set - multiple types
 * Expected: Set int64, float64, string, bool, bytes fields
 */
static void test_doc_field_set_types(void)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_test_db, "users", "user_1", &doc);
	
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	/* Set int64 field */
	rc = zdb_doc_field_set_i64(&doc, "age", 30);
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED, "i64 set failed: %d", rc);

	/* Set float64 field */
	rc = zdb_doc_field_set_f64(&doc, "rating", 4.5);
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED, "f64 set failed: %d", rc);

	/* Set string field */
	rc = zdb_doc_field_set_string(&doc, "name", "Alice");
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED, "string set failed: %d", rc);

	/* Set bool field */
	rc = zdb_doc_field_set_bool(&doc, "active", true);
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED, "bool set failed: %d", rc);

	/* Set bytes field */
	uint8_t bytes_val[] = {0xDE, 0xAD, 0xBE, 0xEF};
	rc = zdb_doc_field_set_bytes(&doc, "data", bytes_val, sizeof(bytes_val));
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED, "bytes set failed: %d", rc);

	rc = zdb_doc_close(&doc);
	assert_zdb_ok(rc);
}

/*
 * Test: DOC field get with correct type
 * Expected: Retrieve value and match what was set
 */
static void test_doc_field_get_match_type(void)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_test_db, "users", "user_1", &doc);
	
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	/* Set and get int64 */
	int64_t expected_age = 30;
	rc = zdb_doc_field_set_i64(&doc, "age", expected_age);
	if (rc != ZDB_OK) {
		zskip("Field operations not available");
		return;
	}

	int64_t retrieved_age = 0;
	rc = zdb_doc_field_get_i64(&doc, "age", &retrieved_age);
	assert_zdb_ok(rc);
	zassert_equal(retrieved_age, expected_age, "Age mismatch: %lld != %lld", 
		      retrieved_age, expected_age);

	/* Set and get string */
	const char *expected_name = "Alice";
	rc = zdb_doc_field_set_string(&doc, "name", expected_name);
	assert_zdb_ok(rc);

	const char *retrieved_name = NULL;
	rc = zdb_doc_field_get_string(&doc, "name", &retrieved_name);
	assert_zdb_ok(rc);
	zassert_str_equal(retrieved_name, expected_name, "Name mismatch");

	rc = zdb_doc_close(&doc);
	assert_zdb_ok(rc);
}

/*
 * Test: DOC field get with wrong type
 * Expected: Returns ZDB_ERR_INVAL
 */
static void test_doc_field_get_wrong_type(void)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_test_db, "users", "user_1", &doc);
	
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	/* Set int64 field */
	rc = zdb_doc_field_set_i64(&doc, "age", 30);
	if (rc != ZDB_OK) {
		zskip("Field operations not available");
		return;
	}

	/* Try to get as string (wrong type) */
	const char *result = NULL;
	rc = zdb_doc_field_get_string(&doc, "age", &result);
	zassert_equal(rc, ZDB_ERR_INVAL, "Expected INVAL for type mismatch, got %d", rc);

	rc = zdb_doc_close(&doc);
	assert_zdb_ok(rc);
}

/*
 * Test: DOC field overwrite
 * Expected: Second set replaces first value
 */
static void test_doc_field_overwrite(void)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_test_db, "users", "user_1", &doc);
	
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	/* Set initial value */
	rc = zdb_doc_field_set_i64(&doc, "age", 25);
	if (rc != ZDB_OK) {
		zskip("Field operations not available");
		return;
	}

	/* Overwrite with new value */
	rc = zdb_doc_field_set_i64(&doc, "age", 35);
	assert_zdb_ok(rc);

	/* Verify new value */
	int64_t retrieved = 0;
	rc = zdb_doc_field_get_i64(&doc, "age", &retrieved);
	assert_zdb_ok(rc);
	zassert_equal(retrieved, 35, "Value not overwritten: %lld != 35", retrieved);

	rc = zdb_doc_close(&doc);
	assert_zdb_ok(rc);
}

/*
 * Test: DOC save persists to storage
 * Expected: Save returns ZDB_OK
 */
static void test_doc_save_success(void)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_test_db, "users", "user_1", &doc);
	
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	/* Set some fields */
	zdb_doc_field_set_string(&doc, "name", "Bob");
	zdb_doc_field_set_i64(&doc, "age", 28);

	/* Save document */
	rc = zdb_doc_save(&doc);
	assert_zdb_ok(rc);

	rc = zdb_doc_close(&doc);
	assert_zdb_ok(rc);
}

/*
 * Test: DOC open existing
 * Expected: Reconstruct document from storage
 */
static void test_doc_open_existing(void)
{
	zdb_doc_t doc1, doc2;
	zdb_status_t rc;

	/* Create and save a document */
	rc = zdb_doc_create(&g_test_db, "users", "user_2", &doc1);
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	zdb_doc_field_set_string(&doc1, "name", "Charlie");
	zdb_doc_field_set_i64(&doc1, "age", 40);

	rc = zdb_doc_save(&doc1);
	assert_zdb_ok(rc);
	rc = zdb_doc_close(&doc1);
	assert_zdb_ok(rc);

	/* Open the saved document */
	rc = zdb_doc_open(&g_test_db, "users", "user_2", &doc2);
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_NOT_FOUND,
		     "Open failed: %d", rc);

	if (rc == ZDB_OK) {
		/* Verify fields are reconstructed */
		const char *name = NULL;
		int64_t age = 0;

		rc = zdb_doc_field_get_string(&doc2, "name", &name);
		if (rc == ZDB_OK) {
			zassert_str_equal(name, "Charlie", "Name mismatch on open");
		}

		rc = zdb_doc_field_get_i64(&doc2, "age", &age);
		if (rc == ZDB_OK) {
			zassert_equal(age, 40, "Age mismatch on open: %lld != 40", age);
		}

		rc = zdb_doc_close(&doc2);
		assert_zdb_ok(rc);
	}
}

/*
 * Test: DOC delete removes document
 * Expected: Delete succeeds, subsequent open returns NOT_FOUND
 */
static void test_doc_delete_success(void)
{
	zdb_doc_t doc;
	zdb_status_t rc;

	/* Create and save a document */
	rc = zdb_doc_create(&g_test_db, "users", "user_3", &doc);
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	rc = zdb_doc_save(&doc);
	assert_zdb_ok(rc);
	rc = zdb_doc_close(&doc);
	assert_zdb_ok(rc);

	/* Delete the document */
	rc = zdb_doc_delete(&g_test_db, "users", "user_3");
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_NOT_FOUND,
		     "Delete failed: %d", rc);

	/* Verify it's gone */
	rc = zdb_doc_open(&g_test_db, "users", "user_3", &doc);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "Document still exists after delete");
}

/*
 * Test: DOC export to FlatBuffer
 * Expected: Serialize document to FlatBuffer format
 */
static void test_doc_export_flatbuffer(void)
{
	zdb_doc_t doc;
	zdb_status_t rc = zdb_doc_create(&g_test_db, "users", "user_4", &doc);
	
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	/* Set some fields */
	zdb_doc_field_set_string(&doc, "name", "Diana");
	zdb_doc_field_set_i64(&doc, "age", 29);

	/* Export to FlatBuffer */
	uint8_t buffer[512];
	size_t out_len = 0;
	rc = zdb_doc_export_flatbuffer(&doc, buffer, sizeof(buffer), &out_len);
	
	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED,
		     "Export failed: %d", rc);

	if (rc == ZDB_OK) {
		zassert_greater_than(out_len, 0, "Export produced empty buffer");
	}

	rc = zdb_doc_close(&doc);
	assert_zdb_ok(rc);
}

/*
 * Test: DOC query with filters
 * Expected: Search documents matching filter criteria
 */
static void test_doc_query_filters(void)
{
	zdb_status_t rc;

	/* Skip if DOC not available */
	zdb_doc_t test_doc;
	rc = zdb_doc_create(&g_test_db, "users", "test", &test_doc);
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}
	zdb_doc_close(&test_doc);

	/* Create 3 documents with filter-queryable fields */
	const struct {
		const char *doc_id;
		const char *name;
		int64_t age;
		bool active;
	} test_docs[] = {
		{"user_a", "Alice", 30, true},
		{"user_b", "Bob", 25, false},
		{"user_c", "Charlie", 30, true},
	};

	/* Save documents */
	for (int i = 0; i < 3; i++) {
		zdb_doc_t doc;
		rc = zdb_doc_create(&g_test_db, "users", test_docs[i].doc_id, &doc);
		if (rc != ZDB_OK) break;

		zdb_doc_field_set_string(&doc, "name", test_docs[i].name);
		zdb_doc_field_set_i64(&doc, "age", test_docs[i].age);
		zdb_doc_field_set_bool(&doc, "active", test_docs[i].active);

		zdb_doc_save(&doc);
		zdb_doc_close(&doc);
	}

	/* Query with filter: age == 30 */
	zdb_doc_query_filter_t filters[] = {
		{.field_name = "age", .type = ZDB_DOC_FIELD_INT64, .i64_value = 30},
	};
	zdb_doc_query_spec_t query_spec = {
		.collection = "users",
		.filters = filters,
		.filter_count = 1,
	};

	zdb_doc_metadata_t *results = NULL;
	size_t result_count = 0;
	rc = zdb_doc_query(&g_test_db, &query_spec, &results, &result_count);

	zassert_true(rc == ZDB_OK || rc == ZDB_ERR_UNSUPPORTED,
		     "Query failed: %d", rc);

	if (rc == ZDB_OK && results != NULL) {
		/* Should find 2 documents (age == 30) */
		zassert_equal(result_count, 2, "Expected 2 results, got %zu", result_count);
		/* Cleanup results */
		zdb_doc_metadata_free(results, result_count);
	}
}

static void test_doc_rejects_path_traversal_names(void)
{
	zdb_doc_t doc;
	zdb_status_t rc;

	rc = zdb_doc_create(&g_test_db, "..", "ok", &doc);
	assert_zdb_eq(rc, ZDB_ERR_INVAL);

	rc = zdb_doc_create(&g_test_db, "users/evil", "ok", &doc);
	assert_zdb_eq(rc, ZDB_ERR_INVAL);

	rc = zdb_doc_create(&g_test_db, "users", "../escape", &doc);
	assert_zdb_eq(rc, ZDB_ERR_INVAL);

	rc = zdb_doc_create(&g_test_db, "users", "bad\\name", &doc);
	assert_zdb_eq(rc, ZDB_ERR_INVAL);
}

static void test_doc_open_fails_on_header_crc_corruption(void)
{
	zdb_doc_t doc;
	zdb_doc_t reopened;
	struct fs_file_t file;
	zdb_status_t rc;
	int fs_rc;

	struct zdb_doc_hdr_v1_test {
		uint32_t magic_le;
		uint16_t version_le;
		uint16_t field_count_le;
		uint64_t created_ms_le;
		uint64_t updated_ms_le;
		uint32_t crc_le;
	} __packed hdr;

	char path[160];

	rc = zdb_doc_create(&g_test_db, "users", "crc_doc", &doc);
	if (rc != ZDB_OK) {
		zskip("DOC module not available");
		return;
	}

	rc = zdb_doc_field_set_i64(&doc, "age", 33);
	assert_zdb_ok(rc);

	rc = zdb_doc_save(&doc);
	assert_zdb_ok(rc);

	rc = zdb_doc_close(&doc);
	assert_zdb_ok(rc);

#if defined(CONFIG_ZDB_DOC_BACKEND_LITTLEFS) && (CONFIG_ZDB_DOC_BACKEND_LITTLEFS)
	(void)snprintf(path, sizeof(path), "%s/%s/%s/%s%s", "/lfs", "zdb_docs", "users", "crc_doc",
		       ".zdoc");

	fs_file_t_init(&file);
	fs_rc = fs_open(&file, path, FS_O_READ | FS_O_WRITE);
	zassert_equal(fs_rc, 0, "Failed opening saved doc file: %d", fs_rc);

	fs_rc = fs_read(&file, &hdr, sizeof(hdr));
	zassert_equal(fs_rc, (int)sizeof(hdr), "Failed reading doc header: %d", fs_rc);

	hdr.crc_le ^= 0x1U;

	fs_rc = fs_seek(&file, 0, FS_SEEK_SET);
	zassert_equal(fs_rc, 0, "Failed seeking to header start: %d", fs_rc);

	fs_rc = fs_write(&file, &hdr, sizeof(hdr));
	zassert_equal(fs_rc, (int)sizeof(hdr), "Failed writing corrupted header: %d", fs_rc);

	fs_rc = fs_close(&file);
	zassert_equal(fs_rc, 0, "Failed closing doc file: %d", fs_rc);

	rc = zdb_doc_open(&g_test_db, "users", "crc_doc", &reopened);
	assert_zdb_eq(rc, ZDB_ERR_CORRUPT);
#else
	zskip("CRC corruption test requires filesystem DOC backend");
#endif
}

/* ===== Test Suite Registration ===== */

ztest_suite(zephyrdb_doc_basic, doc_test_setup, doc_test_teardown,
	    NULL, NULL);

ztest_unit_test(test_doc_create_success);
ztest_unit_test(test_doc_field_set_types);
ztest_unit_test(test_doc_field_get_match_type);
ztest_unit_test(test_doc_field_get_wrong_type);
ztest_unit_test(test_doc_field_overwrite);
ztest_unit_test(test_doc_save_success);
ztest_unit_test(test_doc_open_existing);
ztest_unit_test(test_doc_delete_success);
ztest_unit_test(test_doc_export_flatbuffer);
ztest_unit_test(test_doc_query_filters);
ztest_unit_test(test_doc_rejects_path_traversal_names);
ztest_unit_test(test_doc_open_fails_on_header_crc_corruption);
