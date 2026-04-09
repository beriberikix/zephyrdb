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

ZTEST(zephyrdb_ts, test_ts_open_invalid_args)
{
	zdb_ts_t ts;
	zdb_status_t rc;

	rc = zdb_ts_open(NULL, "metrics", &ts);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval for null db");

	rc = zdb_ts_open(&g_db, NULL, &ts);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval for null stream");

	rc = zdb_ts_open(&g_db, "metrics", NULL);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected inval for null ts handle");
}

ZTEST(zephyrdb_ts, test_ts_open_requires_mount_point)
{
	zdb_ts_t ts;
	zdb_status_t rc;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_ts_open(&g_db, "metrics", &ts);
	zassert_true(rc == ZDB_ERR_INVAL || rc == ZDB_ERR_IO || rc == ZDB_ERR_UNSUPPORTED ||
		     rc == ZDB_ERR_NOMEM,
		     "unexpected status without fs mount: %d", rc);

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "deinit failed: %d", rc);
}

ZTEST_SUITE(zephyrdb_ts, NULL, NULL, NULL, NULL, NULL);
