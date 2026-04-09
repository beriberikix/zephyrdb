#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/zms.h>
#include <zephyr/storage/flash_map.h>

#include "zephyrdb.h"

static struct zms_fs g_zms;

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

#define ZDB_ZMS_PARTITION storage_partition
#define ZDB_ZMS_PARTITION_DEVICE PARTITION_DEVICE(ZDB_ZMS_PARTITION)
#define ZDB_ZMS_PARTITION_OFFSET PARTITION_OFFSET(ZDB_ZMS_PARTITION)

static int shell_basic_init_zms(struct zms_fs *fs)
{
	struct flash_pages_info info;
	int rc;

	fs->flash_device = ZDB_ZMS_PARTITION_DEVICE;
	if (!device_is_ready(fs->flash_device)) {
		printk("shell_basic: storage device not ready\n");
		return -ENODEV;
	}

	fs->offset = ZDB_ZMS_PARTITION_OFFSET;
	rc = flash_get_page_info_by_offs(fs->flash_device, fs->offset, &info);
	if (rc != 0) {
		printk("shell_basic: flash page info failed rc=%d\n", rc);
		return rc;
	}

	fs->sector_size = info.size;
	fs->sector_count = 3U;

	rc = zms_mount(fs);
	if (rc != 0) {
		printk("shell_basic: zms mount failed rc=%d\n", rc);
		return rc;
	}

	return 0;
}

static int shell_basic_kv_selftest(zdb_t *db)
{
	zdb_kv_t kv;
	zdb_status_t rc;
	uint32_t write_value = 7U;
	uint32_t read_value = 0U;
	size_t out_len = 0U;

	rc = zdb_kv_open(db, "shell", &kv);
	if (rc != ZDB_OK) {
		printk("shell_basic: selftest kv_open rc=%d\n", (int)rc);
		return -EIO;
	}

	rc = zdb_kv_set(&kv, "smoke", &write_value, sizeof(write_value));
	if (rc != ZDB_OK) {
		printk("shell_basic: selftest kv_set rc=%d\n", (int)rc);
		(void)zdb_kv_close(&kv);
		return -EIO;
	}

	rc = zdb_kv_get(&kv, "smoke", &read_value, sizeof(read_value), &out_len);
	if ((rc != ZDB_OK) || (out_len != sizeof(read_value)) || (read_value != write_value)) {
		printk("shell_basic: selftest kv_get rc=%d len=%u val=%u\n", (int)rc,
		       (unsigned int)out_len, (unsigned int)read_value);
		(void)zdb_kv_close(&kv);
		return -EIO;
	}

	rc = zdb_kv_delete(&kv, "smoke");
	if (rc != ZDB_OK) {
		printk("shell_basic: selftest kv_delete rc=%d\n", (int)rc);
		(void)zdb_kv_close(&kv);
		return -EIO;
	}

	(void)zdb_kv_close(&kv);
	printk("shell_basic: KV selftest PASS\n");

	return 0;
}

int main(void)
{
	zdb_status_t rc;
	int zms_rc;
	int selftest_rc;

	zms_rc = shell_basic_init_zms(&g_zms);
	if (zms_rc != 0) {
		return 1;
	}

	g_cfg.kv_backend_fs = &g_zms;

	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("shell_basic: zdb_init failed rc=%d\n", (int)rc);
		return 1;
	}

	selftest_rc = shell_basic_kv_selftest(&g_db);
	if (selftest_rc != 0) {
		return 1;
	}

	zdb_shell_register(&g_db);
	printk("shell_basic: ZephyrDB shell ready. Try: zdb health, zdb stats\n");

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
