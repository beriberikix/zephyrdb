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
    .kv_namespace = "doc_query_filters",
    .work_q = &k_sys_work_q,
    .scan_yield_every_n = CONFIG_ZDB_SCAN_YIELD_EVERY_N,
};

int main(void)
{
#if !defined(CONFIG_ZDB_DOC) || !(CONFIG_ZDB_DOC)
    printk("DOC query helper: CONFIG_ZDB_DOC is disabled for this board/config.\n");
    return 0;
#else
    zdb_status_t rc;
    zdb_doc_t doc;
    zdb_doc_query_filter_t filters[2];
    zdb_doc_query_t query;
    zdb_doc_metadata_t results[8];
    size_t result_count = ARRAY_SIZE(results);

    g_db.core_slab = &g_core_slab;
    g_db.cursor_slab = &g_cursor_slab;
    g_db.kv_io_slab = NULL;
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
    g_db.ts_ingest_slab = &g_ts_ingest_slab;
#else
    g_db.ts_ingest_slab = NULL;
#endif

    rc = zdb_init(&g_db, &g_cfg);
    if (rc != ZDB_OK) {
        printk("DOC query helper: init failed rc=%d\n", (int)rc);
        return 1;
    }

    /* Seed one document for query demonstration. */
    rc = zdb_doc_create(&g_db, "users", "u100", &doc);
    if (rc == ZDB_OK) {
        (void)zdb_doc_field_set_string(&doc, "name", "Ada");
        (void)zdb_doc_field_set_bool(&doc, "active", true);
        rc = zdb_doc_save(&doc);
        (void)zdb_doc_close(&doc);
        if (rc != ZDB_OK) {
            printk("DOC query helper: save failed rc=%d\n", (int)rc);
            printk("DOC query helper: ensure the filesystem is mounted/configured at %s\n",
                   CONFIG_ZDB_LFS_MOUNT_POINT);
            (void)zdb_deinit(&g_db);
            return 1;
        }
    }

    filters[0].field_name = "name";
    filters[0].type = ZDB_DOC_FIELD_STRING;
    filters[0].string_value = "Ada";
    filters[0].numeric_value = 0.0;

    filters[1].field_name = "active";
    filters[1].type = ZDB_DOC_FIELD_BOOL;
    filters[1].numeric_value = 1.0;
    filters[1].string_value = NULL;

    query.filters = filters;
    query.filter_count = ARRAY_SIZE(filters);
    query.from_ms = 0U;
    query.to_ms = UINT64_MAX;
    query.limit = (uint32_t)ARRAY_SIZE(results);

    rc = zdb_doc_query(&g_db, &query, results, &result_count);
    if (rc == ZDB_OK) {
        if (result_count > 0U) {
            printk("DOC query helper PASS: matched=%u\n", (unsigned)result_count);
            zdb_doc_metadata_free(results, result_count);
        } else {
            printk("DOC query helper: query returned 0 matches; document storage may be empty or unavailable\n");
        }
    } else {
        printk("DOC query helper: query rc=%d\n", (int)rc);
    }

    (void)zdb_deinit(&g_db);
    return 0;
#endif
}
