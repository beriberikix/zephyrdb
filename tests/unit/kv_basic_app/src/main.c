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
};

ZTEST(zephyrdb_kv_basic, test_kv_open_without_backend_fails)
{
	zdb_status_t rc;
	zdb_kv_t kv;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "app", &kv);
	zassert_equal(rc, ZDB_ERR_INVAL, "kv_open should fail without backend: %d", rc);

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "deinit failed: %d", rc);
}

ZTEST(zephyrdb_kv_basic, test_kv_invalid_args)
{
	zdb_kv_t kv;
	zassert_equal(zdb_kv_open(NULL, "a", &kv), ZDB_ERR_INVAL, "null db should fail");
	zassert_equal(zdb_kv_open(&g_db, NULL, &kv), ZDB_ERR_INVAL, "null namespace should fail");
	zassert_equal(zdb_kv_open(&g_db, "a", NULL), ZDB_ERR_INVAL, "null handle should fail");
}

ZTEST_SUITE(zephyrdb_kv_basic, NULL, NULL, NULL, NULL, NULL);
