#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "zephyrdb.h"

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = NULL,
	.work_q = &k_sys_work_q,
};

K_MEM_SLAB_DEFINE_STATIC(g_core_slab, 128, 16, 4);
K_MEM_SLAB_DEFINE_STATIC(g_cursor_slab, 96, 8, 4);

static zdb_t g_db = {
	.core_slab = &g_core_slab,
	.cursor_slab = &g_cursor_slab,
	.kv_io_slab = NULL,
	.ts_ingest_slab = NULL,
};

ZTEST(zephyrdb_kv, test_kv_open_invalid_args)
{
	zdb_kv_t kv;
	zdb_status_t rc;

	rc = zdb_kv_open(NULL, "ns", &kv);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval for null db");

	rc = zdb_kv_open(&g_db, NULL, &kv);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval for null namespace");

	rc = zdb_kv_open(&g_db, "ns", NULL);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval for null kv handle");
}

ZTEST(zephyrdb_kv, test_kv_open_requires_backend)
{
	zdb_kv_t kv;
	zdb_status_t rc;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "ns", &kv);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval without kv backend fs");

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "deinit failed: %d", rc);
}

ZTEST_SUITE(zephyrdb_kv, NULL, NULL, NULL, NULL, NULL);
