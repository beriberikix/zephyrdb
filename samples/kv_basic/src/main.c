#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "zephyrdb.h"

ZDB_DEFINE_CORE_SLAB(g_core_slab);
ZDB_DEFINE_CURSOR_SLAB(g_cursor_slab);
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
ZDB_DEFINE_KV_IO_SLAB(g_kv_io_slab);
#endif

static zdb_t g_db;

static const zdb_cfg_t g_cfg = {
	.partition_ref = NULL,
	.lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
	.kv_namespace = "kv_basic",
	.work_q = &k_sys_work_q,
	.scan_yield_every_n = CONFIG_ZDB_SCAN_YIELD_EVERY_N,
};

int main(void)
{
#if !defined(CONFIG_ZDB_KV) || !(CONFIG_ZDB_KV)
	printk("KV sample: CONFIG_ZDB_KV is disabled for this board/config.\n");
	return 0;
#else
	zdb_status_t rc;
	zdb_kv_t kv;
	uint32_t value = 42U;
	uint32_t readback = 0U;
	size_t out_len = 0U;

	g_db.core_slab = &g_core_slab;
	g_db.cursor_slab = &g_cursor_slab;
	g_db.kv_io_slab = &g_kv_io_slab;
	g_db.ts_ingest_slab = NULL;

	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("KV sample: zdb_init failed rc=%d\n", (int)rc);
		return 1;
	}

	rc = zdb_kv_open(&g_db, "app", &kv);
	if (rc != ZDB_OK) {
		printk("KV sample: zdb_kv_open failed rc=%d (configure a board/storage partition for NVS)\n",
		       (int)rc);
		(void)zdb_deinit(&g_db);
		return 0;
	}

	rc = zdb_kv_set(&kv, "boot_count", &value, sizeof(value));
	if (rc == ZDB_OK) {
		rc = zdb_kv_get(&kv, "boot_count", &readback, sizeof(readback), &out_len);
	}

	if ((rc == ZDB_OK) && (out_len == sizeof(readback))) {
		printk("KV sample PASS: boot_count=%u\n", readback);
	} else {
		printk("KV sample: KV operations returned rc=%d\n", (int)rc);
	}

	(void)zdb_kv_close(&kv);
	(void)zdb_deinit(&g_db);
	return 0;
#endif
}
