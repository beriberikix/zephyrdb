#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "zephyrdb.h"

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

int main(void)
{
	zdb_status_t rc;
	zdb_ts_stats_t ts_stats;
	zdb_health_t health;

	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("Core helper: init failed rc=%d\n", (int)rc);
		return 1;
	}

	health = zdb_health(&g_db);
	zdb_ts_stats_get(&g_db, &ts_stats);
	printk("Core helper PASS: health=%d recover_runs=%u crc_fail=%u\n",
	       (int)health,
	       ts_stats.recover_runs,
	       ts_stats.crc_failures);

	zdb_ts_stats_reset(&g_db);
	(void)zdb_deinit(&g_db);
	return 0;
}
