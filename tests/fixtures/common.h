/*
 * Copyright (c) 2026 Test Suite
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYRDB_TEST_COMMON_H
#define ZEPHYRDB_TEST_COMMON_H

#include <zephyr/ztest.h>
#include <zephyr/kernel.h>
#include <zephyrdb.h>
#include <string.h>

/* ===== Memory Setup ===== */

/*
 * Macro to define static memory slabs for test instances.
 * Creates slabs sized for testing (smaller than production).
 */
#define ZDB_TEST_SLABS_DEFINE(instance_name)                                  \
	ZDB_DEFINE_CORE_SLAB(instance_name##_core_slab);                       \
	ZDB_DEFINE_CURSOR_SLAB(instance_name##_cursor_slab);                   \
	ZDB_DEFINE_KV_IO_SLAB(instance_name##_kv_io_slab);                     \
	ZDB_DEFINE_TS_INGEST_SLAB(instance_name##_ts_ingest_slab)

/*
 * Macro to declare a static database instance with pre-allocated slabs.
 * Usage: ZDB_TEST_INSTANCE_DEFINE(my_db);
 */
#define ZDB_TEST_INSTANCE_DEFINE(name)                                        \
	ZDB_TEST_SLABS_DEFINE(name);                                           \
	static zdb_t name = {                                                   \
		.core_slab = &name##_core_slab,                                \
		.cursor_slab = &name##_cursor_slab,                           \
		.kv_io_slab = &name##_kv_io_slab,                             \
		.ts_ingest_slab = &name##_ts_ingest_slab,                    \
	}

/* ===== Mock Backends ===== */

/*
 * Fake NVS backend for unit testing.
 * Provides simple hash-table-like storage without real flash.
 */
struct mock_nvs_fs {
	struct nvs_fs base;
	/* Storage: 10 simple key-value pairs max */
	uint32_t keys[10];
	uint8_t values[10][256];
	uint16_t value_lens[10];
	uint8_t count;
};

/*
 * Initialize a mock NVS backend.
 */
int mock_nvs_init(struct mock_nvs_fs *mock_nvs);

/*
 * Reset mock NVS to empty state.
 */
void mock_nvs_reset(struct mock_nvs_fs *mock_nvs);

/*
 * Fake LittleFS backend for TS testing.
 * Provides simple file operations without real flash.
 */
struct mock_lfs_fs {
	/* Storage: files indexed by name hash */
	struct {
		char name[32];
		uint8_t data[4096];
		size_t size;
		bool exists;
	} files[5];
	uint8_t file_count;
};

/*
 * Initialize a mock LittleFS backend.
 */
int mock_lfs_init(struct mock_lfs_fs *mock_lfs);

/*
 * Reset mock LittleFS to empty state.
 */
void mock_lfs_reset(struct mock_lfs_fs *mock_lfs);

/* ===== Assertion Helpers ===== */

/*
 * Assert that a zdb_status_t equals expected value.
 * Usage: assert_zdb_ok(rc);
 *        assert_zdb_eq(rc, ZDB_ERR_NOT_FOUND);
 */
#define assert_zdb_ok(rc)   zassert_equal(rc, ZDB_OK, "Expected ZDB_OK, got %d", rc)
#define assert_zdb_eq(rc, expected)                                            \
	zassert_equal(rc, expected, "Expected %d, got %d", expected, rc)

/*
 * Assert that memory slab allocation succeeded.
 */
#define assert_slab_alloc_ok(ptr) zassert_not_null(ptr, "Slab allocation failed")

/* ===== Test Lifecycle ===== */

/*
 * Initialize a database instance for testing with mock backends.
 * Sets up configuration with mocked NVS and LittleFS.
 *
 * Returns: ZDB_OK on success, or a zdb_status_t error code from zdb_init()
 * on failure.
 */
int zdb_test_init(zdb_t *db, struct k_work_q *work_q);

/*
 * Cleanup database resources after a test.
 * Calls zdb_deinit.
 *
 * Returns: 0 on completion
 */
int zdb_test_cleanup(zdb_t *db);

/* ===== Helper Macros ===== */

/*
 * Wrapper Zephyr ztest_main pattern for consistent test entry.
 * Usage:
 *   ZDB_TEST_SUITE(foo_tests, {
 *       ztest_test_suite(foo, ztest_unit_test(test_foo_1), ...);
 *   });
 * Expands to standard ztest_main().
 */
#define ZDB_TEST_SUITE(suite_name, test_list)                                 \
	ztest_test_suite(suite_name, test_list);                               \
	static int run_##suite_name(const struct ztest_unit_test *test,       \
				   void *userdata)                              \
	{                                                                        \
		ztest_run_registered_tests();                                  \
		return 0;                                                       \
	}

/* ===== Common Test Data ===== */

/* Standard test key-value pairs. */
static const struct {
	const char *key;
	const uint8_t *value;
	size_t value_len;
} zdb_test_kvs[] = {
	{"boot_count", (const uint8_t *)"\x01\x00\x00\x00", 4},
	{"device_id", (const uint8_t *)"\x42\x00\x00\x00", 4},
	{"config_v", (const uint8_t *)"\x02\x00\x00\x00", 4},
};

#define ZDB_TEST_KV_COUNT (sizeof(zdb_test_kvs) / sizeof(zdb_test_kvs[0]))

#endif /* ZEPHYRDB_TEST_COMMON_H */
