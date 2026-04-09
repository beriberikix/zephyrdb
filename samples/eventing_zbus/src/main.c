#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/zms.h>
#include <zephyr/storage/flash_map.h>

#include <errno.h>

#include "zephyrdb.h"
#include "zephyrdb_eventing_zbus.h"

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

static const char *event_type_str(zdb_event_type_t type)
{
	switch (type) {
	case ZDB_EVENT_KV_SET:
		return "SET";
	case ZDB_EVENT_KV_DELETE:
		return "DELETE";
	default:
		return "UNKNOWN";
	}
}

static int init_zms(struct zms_fs *fs)
{
	struct flash_pages_info info;
	int rc;

	fs->flash_device = ZDB_ZMS_PARTITION_DEVICE;
	if (!device_is_ready(fs->flash_device)) {
		printk("eventing_zbus: storage device not ready\n");
		return -ENODEV;
	}

	fs->offset = ZDB_ZMS_PARTITION_OFFSET;
	rc = flash_get_page_info_by_offs(fs->flash_device, fs->offset, &info);
	if (rc != 0) {
		printk("eventing_zbus: flash page info failed rc=%d\n", rc);
		return rc;
	}

	fs->sector_size = info.size;
	fs->sector_count = 3U;

	rc = zms_mount(fs);
	if (rc != 0) {
		printk("eventing_zbus: zms mount failed rc=%d\n", rc);
		return rc;
	}

	return 0;
}

static int print_latest_event(const char *label)
{
	zdb_kv_event_t event;
	int rc;

	rc = zbus_chan_read(&zdb_kv_event_chan, &event, K_NO_WAIT);
	if (rc != 0) {
		printk("eventing_zbus: %s read failed rc=%d\n", label, rc);
		return rc;
	}

	printk("eventing_zbus: %s type=%s ns=%s key=%s len=%u status=%d ts_ms=%llu\n", label,
	       event_type_str(event.type),
	       (event.namespace_name != NULL) ? event.namespace_name : "(null)",
	       (event.key != NULL) ? event.key : "(null)",
	       (unsigned int)event.value_len, (int)event.status,
	       (unsigned long long)event.timestamp_ms);
	return 0;
}

int main(void)
{
	zdb_kv_t kv;
	zdb_status_t rc;
	int zms_rc;
	int event_rc;
	uint32_t value = 123U;

	zms_rc = init_zms(&g_zms);
	if (zms_rc != 0) {
		return 1;
	}

	g_cfg.kv_backend_fs = &g_zms;

	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("eventing_zbus: zdb_init failed rc=%d\n", (int)rc);
		return 1;
	}

	rc = zdb_kv_open(&g_db, "sample", &kv);
	if (rc != ZDB_OK) {
		printk("eventing_zbus: kv_open failed rc=%d\n", (int)rc);
		return 1;
	}

	rc = zdb_kv_set(&kv, "counter", &value, sizeof(value));
	if (rc != ZDB_OK) {
		printk("eventing_zbus: kv_set failed rc=%d\n", (int)rc);
		return 1;
	}

	event_rc = print_latest_event("event after set");
	if (event_rc != 0) {
		return 1;
	}

	rc = zdb_kv_delete(&kv, "counter");
	if (rc != ZDB_OK) {
		printk("eventing_zbus: kv_delete failed rc=%d\n", (int)rc);
		return 1;
	}

	event_rc = print_latest_event("event after delete");
	if (event_rc != 0) {
		return 1;
	}

	printk("eventing_zbus: PASS\n");
	(void)zdb_kv_close(&kv);
	(void)zdb_deinit(&g_db);
	return 0;
}
