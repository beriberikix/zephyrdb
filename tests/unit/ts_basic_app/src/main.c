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

ZTEST(zephyrdb_ts_basic, test_ts_invalid_args)
{
	zdb_ts_t ts;
	zassert_equal(zdb_ts_open(NULL, "metrics", &ts), ZDB_ERR_INVAL, "null db should fail");
	zassert_equal(zdb_ts_open(&g_db, NULL, &ts), ZDB_ERR_INVAL, "null stream should fail");
	zassert_equal(zdb_ts_open(&g_db, "metrics", NULL), ZDB_ERR_INVAL, "null handle should fail");
}

ZTEST(zephyrdb_ts_basic, test_ts_open_without_fs_is_graceful)
{
	zdb_status_t rc;
	zdb_ts_t ts;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_ts_open(&g_db, "metrics", &ts);
	zassert_true(rc == ZDB_ERR_INVAL || rc == ZDB_ERR_IO || rc == ZDB_ERR_UNSUPPORTED || rc == ZDB_ERR_NOMEM,
		     "unexpected ts open rc=%d", rc);

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "deinit failed: %d", rc);
}

ZTEST_SUITE(zephyrdb_ts_basic, NULL, NULL, NULL, NULL, NULL);
