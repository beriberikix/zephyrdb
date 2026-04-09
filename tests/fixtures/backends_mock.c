/*
 * Copyright (c) 2026 Test Suite
 * SPDX-License-Identifier: Apache-2.0
 */

#include "common.h"

/* ===== Mock NVS Backend ===== */

int mock_nvs_init(struct mock_nvs_fs *mock_nvs)
{
	if (!mock_nvs) {
		return -EINVAL;
	}
	mock_nvs->count = 0;
	memset(mock_nvs->keys, 0, sizeof(mock_nvs->keys));
	memset(mock_nvs->values, 0, sizeof(mock_nvs->values));
	memset(mock_nvs->value_lens, 0, sizeof(mock_nvs->value_lens));
	return 0;
}

void mock_nvs_reset(struct mock_nvs_fs *mock_nvs)
{
	if (mock_nvs) {
		mock_nvs->count = 0;
	}
}

/*
 * Mock NVS operations (simplified for testing).
 * Only supports basic set/get/delete without real persistence.
 */

/* ===== Mock LittleFS Backend ===== */

int mock_lfs_init(struct mock_lfs_fs *mock_lfs)
{
	if (!mock_lfs) {
		return -EINVAL;
	}
	mock_lfs->file_count = 0;
	memset(mock_lfs->files, 0, sizeof(mock_lfs->files));
	return 0;
}

void mock_lfs_reset(struct mock_lfs_fs *mock_lfs)
{
	if (mock_lfs) {
		mock_lfs->file_count = 0;
		memset(mock_lfs->files, 0, sizeof(mock_lfs->files));
	}
}

/*
 * Mock LittleFS operations (simplified for testing).
 * Provides basic file read/write without real flash.
 */

/* ===== Test Lifecycle Helpers ===== */

int zdb_test_init(zdb_t *db, struct k_work_q *work_q)
{
	if (!db) {
		return -EINVAL;
	}

	/* Default to system workqueue if not provided */
	if (!work_q) {
		work_q = &k_sys_work_q;
	}

	/* Configure database with mock backends */
	zdb_cfg_t cfg = {
		.kv_backend_fs = NULL,  /* Will be set by test */
		.lfs_mount_point = "/lfs",
		.work_q = work_q,
	};

	/* Initialize database */
	int rc = zdb_init(db, &cfg);
	if (rc != ZDB_OK) {
		return rc;
	}

	return 0;
}

int zdb_test_cleanup(zdb_t *db)
{
	if (!db) {
		return -EINVAL;
	}

	/* Deinitialize database */
	zdb_deinit(db);

	/* TODO: Check for slab memory leaks */
	/* This would require introspecting slab state, which may not be exposed */

	return 0;
}
