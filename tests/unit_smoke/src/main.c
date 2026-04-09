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

ZTEST(zephyrdb_core, test_init_deinit_ok)
{
	zdb_status_t rc;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "zdb_init failed: %d", rc);

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "zdb_deinit failed: %d", rc);
}

ZTEST(zephyrdb_core, test_init_invalid_args)
{
	zdb_status_t rc;

	rc = zdb_init(NULL, &g_cfg);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected ZDB_ERR_INVAL, got %d", rc);

	rc = zdb_init(&g_db, NULL);
	zassert_equal(rc, ZDB_ERR_INVAL, "expected ZDB_ERR_INVAL, got %d", rc);
}

ZTEST(zephyrdb_core, test_health_and_stats)
{
	zdb_status_t rc;
	zdb_health_t health;
	zdb_ts_stats_t stats;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "zdb_init failed: %d", rc);

	health = zdb_health(&g_db);
	zassert_true(health == ZDB_HEALTH_OK || health == ZDB_HEALTH_DEGRADED ||
		     health == ZDB_HEALTH_READONLY || health == ZDB_HEALTH_FAULT,
		     "unexpected health value: %d", health);

	zdb_ts_stats_get(&g_db, &stats);
	zassert_true(stats.recover_runs == 0U, "unexpected initial recover_runs");

	zdb_ts_stats_reset(&g_db);
	zdb_ts_stats_get(&g_db, &stats);
	zassert_true(stats.recover_runs == 0U, "reset did not keep zero recover_runs");

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "zdb_deinit failed: %d", rc);
}

ZTEST_SUITE(zephyrdb_core, NULL, NULL, NULL, NULL, NULL);
