#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "zephyrdb.h"

ZDB_DEFINE_CORE_SLAB(g_core_slab);
ZDB_DEFINE_CURSOR_SLAB(g_cursor_slab);

static zdb_t g_db;

static const zdb_cfg_t g_cfg = {
	.partition_ref = NULL,
	.lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
	.kv_namespace = "core_health_stats",
	.work_q = &k_sys_work_q,
	.scan_yield_every_n = CONFIG_ZDB_SCAN_YIELD_EVERY_N,
};

int main(void)
{
	zdb_status_t rc;
	zdb_stats_t stats;
	zdb_health_t health;

	g_db.core_slab = &g_core_slab;
	g_db.cursor_slab = &g_cursor_slab;
	g_db.kv_io_slab = NULL;
	g_db.ts_ingest_slab = NULL;

	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("Core helper: init failed rc=%d\n", (int)rc);
		return 1;
	}

	health = zdb_health(&g_db);
	zdb_stats_get(&g_db, &stats);
	printk("Core helper PASS: health=%d recover_runs=%u crc_fail=%u\n",
	       (int)health,
	       stats.ts_recover_runs,
	       stats.ts_crc_failures);

	zdb_stats_reset(&g_db);
	(void)zdb_deinit(&g_db);
	return 0;
}
