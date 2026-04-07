#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "zephyrdb.h"

ZDB_DEFINE_CORE_SLAB(g_core_slab);
ZDB_DEFINE_CURSOR_SLAB(g_cursor_slab);
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
ZDB_DEFINE_TS_INGEST_SLAB(g_ts_ingest_slab);
#endif

static zdb_t g_db;

static const zdb_cfg_t g_cfg = {
    .partition_ref = NULL,
    .lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
    .kv_namespace = "ts_basic",
    .work_q = &k_sys_work_q,
    .scan_yield_every_n = CONFIG_ZDB_SCAN_YIELD_EVERY_N,
};

int main(void)
{
#if !defined(CONFIG_ZDB_TS) || !(CONFIG_ZDB_TS)
    printk("TS sample: CONFIG_ZDB_TS is disabled for this board/config.\n");
    return 0;
#else
    zdb_status_t rc;
    zdb_ts_t stream;
    zdb_ts_sample_i64_t sample = {
        .ts_ms = (uint64_t)k_uptime_get(),
        .value = 100,
    };
    zdb_ts_agg_result_t agg;
    zdb_ts_window_t window = {
        .from_ts_ms = 0U,
        .to_ts_ms = UINT64_MAX,
    };

    g_db.core_slab = &g_core_slab;
    g_db.cursor_slab = &g_cursor_slab;
    g_db.kv_io_slab = NULL;
    g_db.ts_ingest_slab = &g_ts_ingest_slab;

    rc = zdb_init(&g_db, &g_cfg);
    if (rc != ZDB_OK) {
        printk("TS sample: zdb_init failed rc=%d\n", (int)rc);
        return 1;
    }

    rc = zdb_ts_open(&g_db, "metrics", &stream);
    if (rc != ZDB_OK) {
        printk("TS sample: zdb_ts_open failed rc=%d (configure/mount filesystem at %s)\n",
               (int)rc, g_cfg.lfs_mount_point);
        (void)zdb_deinit(&g_db);
        return 0;
    }

    (void)zdb_ts_append_i64(&stream, &sample);
    (void)zdb_ts_flush_sync(&stream, K_SECONDS(2));

    rc = zdb_ts_query_aggregate(&stream, window, ZDB_TS_AGG_AVG, &agg);
    if (rc == ZDB_OK) {
        printk("TS sample PASS: avg=%f points=%u\n", agg.value, agg.points);
    } else {
        printk("TS sample: aggregate query returned rc=%d\n", (int)rc);
    }

    (void)zdb_ts_close(&stream);
    (void)zdb_deinit(&g_db);
    return 0;
#endif
}
