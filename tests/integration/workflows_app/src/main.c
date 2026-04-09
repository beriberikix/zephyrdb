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

ZTEST(zephyrdb_workflows, test_init_health_deinit)
{
	zdb_status_t rc;
	zdb_health_t health;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	health = zdb_health(&g_db);
	zassert_true(health == ZDB_HEALTH_OK || health == ZDB_HEALTH_DEGRADED ||
		     health == ZDB_HEALTH_READONLY || health == ZDB_HEALTH_FAULT,
		     "unexpected health state: %d", health);

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "deinit failed: %d", rc);
}

ZTEST(zephyrdb_workflows, test_kv_and_ts_fail_gracefully_without_backends)
{
	zdb_status_t rc;
	zdb_kv_t kv;
	zdb_ts_t ts;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_kv_open(&g_db, "app", &kv);
	zassert_equal(rc, ZDB_ERR_INVAL, "kv open should fail without backend: %d", rc);

	rc = zdb_ts_open(&g_db, "metrics", &ts);
	zassert_true(rc == ZDB_ERR_INVAL || rc == ZDB_ERR_IO || rc == ZDB_ERR_UNSUPPORTED || rc == ZDB_ERR_NOMEM,
		     "unexpected ts open rc=%d", rc);

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "deinit failed: %d", rc);
}

ZTEST_SUITE(zephyrdb_workflows, NULL, NULL, NULL, NULL, NULL);
