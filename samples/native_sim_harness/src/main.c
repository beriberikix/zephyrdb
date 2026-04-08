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
	zdb_ts_sample_i64_t sample = {
		.ts_ms = (uint64_t)k_uptime_get(),
		.value = 123,
	};
	size_t fb_len = 0U;
	uint8_t fb_buf[64];
	zdb_ts_stats_t ts_stats;

	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("FAIL: zdb_init rc=%d\n", (int)rc);
		return 1;
	}

	/* Stage 2 bootstrap: size query + export FlatBuffer payload. */
	rc = zdb_ts_sample_i64_export_flatbuffer(&sample, NULL, 0U, &fb_len);
	if ((rc != ZDB_OK) || (fb_len == 0U) || (fb_len > sizeof(fb_buf))) {
		printk("FAIL: size query rc=%d len=%u\n", (int)rc, (unsigned)fb_len);
		(void)zdb_deinit(&g_db);
		return 1;
	}

	rc = zdb_ts_sample_i64_export_flatbuffer(&sample, fb_buf, sizeof(fb_buf), &fb_len);
	if ((rc != ZDB_OK) || (fb_len == 0U)) {
		printk("FAIL: export rc=%d len=%u\n", (int)rc, (unsigned)fb_len);
		(void)zdb_deinit(&g_db);
		return 1;
	}

	zdb_ts_stats_get(&g_db, &ts_stats);
	printk("PASS: native_sim harness exported %u bytes\n", (unsigned)fb_len);
	ARG_UNUSED(ts_stats);

	(void)zdb_deinit(&g_db);
	return 0;
}
