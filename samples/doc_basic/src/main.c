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
#if !defined(CONFIG_ZDB_DOC) || !(CONFIG_ZDB_DOC)
    printk("DOC sample: CONFIG_ZDB_DOC is disabled for this board/config.\n");
    return 0;
#else
    zdb_status_t rc;
    zdb_doc_t doc;
    const char *name = NULL;
    size_t fb_len = 0U;
    uint8_t fb_buf[256];

    rc = zdb_init(&g_db, &g_cfg);
    if (rc != ZDB_OK) {
        printk("DOC sample: zdb_init failed rc=%d\n", (int)rc);
        return 1;
    }

    rc = zdb_doc_create(&g_db, "users", "u1", &doc);
    if (rc != ZDB_OK) {
        printk("DOC sample: create failed rc=%d\n", (int)rc);
        (void)zdb_deinit(&g_db);
        return 1;
    }

    (void)zdb_doc_field_set_string(&doc, "name", "Ada");
    (void)zdb_doc_field_set_i64(&doc, "age", 32);
    (void)zdb_doc_field_set_bool(&doc, "active", true);

    rc = zdb_doc_save(&doc);
    if (rc != ZDB_OK) {
        printk("DOC sample: save returned rc=%d (configure/mount filesystem at %s)\n",
               (int)rc, g_cfg.lfs_mount_point);
        (void)zdb_doc_close(&doc);
        (void)zdb_deinit(&g_db);
        return 0;
    }

    rc = zdb_doc_field_get_string(&doc, "name", &name);
    if (rc == ZDB_OK) {
        printk("DOC sample: name=%s\n", name);
    }

    rc = zdb_doc_export_flatbuffer(&doc, fb_buf, sizeof(fb_buf), &fb_len);
    if (rc == ZDB_OK) {
        printk("DOC sample PASS: exported %u bytes\n", (unsigned)fb_len);
    } else {
        printk("DOC sample: export rc=%d\n", (int)rc);
    }

    (void)zdb_doc_close(&doc);
    (void)zdb_deinit(&g_db);
    return 0;
#endif
}
