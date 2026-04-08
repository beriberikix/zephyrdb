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
#if !defined(CONFIG_ZDB_KV) || !(CONFIG_ZDB_KV)
	printk("KV sample: CONFIG_ZDB_KV is disabled for this board/config.\n");
	return 0;
#else
	zdb_status_t rc;
	zdb_kv_t kv;
	uint32_t value = 42U;
	uint32_t readback = 0U;
	size_t out_len = 0U;
	zdb_status_t delete_rc;

	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("KV sample: zdb_init failed rc=%d\n", (int)rc);
		return 1;
	}

	rc = zdb_kv_open(&g_db, "app", &kv);
	if (rc != ZDB_OK) {
		printk("KV sample: zdb_kv_open failed rc=%d (configure mounted NVS/ZMS kv_backend_fs in cfg)\n",
		       (int)rc);
		(void)zdb_deinit(&g_db);
		return 0;
	}

	rc = zdb_kv_set(&kv, "boot_count", &value, sizeof(value));
	if (rc == ZDB_OK) {
		rc = zdb_kv_get(&kv, "boot_count", &readback, sizeof(readback), &out_len);
	}

	if ((rc == ZDB_OK) && (out_len == sizeof(readback))) {
		delete_rc = zdb_kv_delete(&kv, "boot_count");
		if (delete_rc == ZDB_OK) {
			rc = zdb_kv_get(&kv, "boot_count", &readback, sizeof(readback), &out_len);
			if (rc == ZDB_ERR_NOT_FOUND) {
				printk("KV sample PASS: boot_count=%u and delete verified\n", value);
			} else {
				printk("KV sample: delete verification returned rc=%d\n", (int)rc);
			}
		} else {
			printk("KV sample: delete returned rc=%d\n", (int)delete_rc);
		}
	} else {
		printk("KV sample: KV operations returned rc=%d\n", (int)rc);
	}

	(void)zdb_kv_close(&kv);
	(void)zdb_deinit(&g_db);
	return 0;
#endif
}
