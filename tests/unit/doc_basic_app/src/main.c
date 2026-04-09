#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "zephyrdb.h"

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = NULL,
	.work_q = &k_sys_work_q,
};

K_MEM_SLAB_DEFINE_STATIC(g_core_slab, 128, 16, 4);
K_MEM_SLAB_DEFINE_STATIC(g_cursor_slab, 96, 8, 4);

static zdb_t g_db = {
	.core_slab = &g_core_slab,
	.cursor_slab = &g_cursor_slab,
};

ZTEST(zephyrdb_doc_basic, test_doc_create_without_mount_fails)
{
	zdb_status_t rc;
	zdb_doc_t doc;

	rc = zdb_init(&g_db, &g_cfg);
	zassert_equal(rc, ZDB_OK, "init failed: %d", rc);

	rc = zdb_doc_create(&g_db, "users", "u1", &doc);
	zassert_equal(rc, ZDB_ERR_INVAL, "doc create should fail without mount: %d", rc);

	rc = zdb_deinit(&g_db);
	zassert_equal(rc, ZDB_OK, "deinit failed: %d", rc);
}

ZTEST(zephyrdb_doc_basic, test_doc_invalid_args)
{
	zdb_doc_t doc;
	zassert_equal(zdb_doc_create(NULL, "c", "d", &doc), ZDB_ERR_INVAL, "null db should fail");
	zassert_equal(zdb_doc_create(&g_db, NULL, "d", &doc), ZDB_ERR_INVAL, "null collection should fail");
	zassert_equal(zdb_doc_create(&g_db, "c", NULL, &doc), ZDB_ERR_INVAL, "null id should fail");
	zassert_equal(zdb_doc_create(&g_db, "c", "d", NULL), ZDB_ERR_INVAL, "null out doc should fail");
}

ZTEST_SUITE(zephyrdb_doc_basic, NULL, NULL, NULL, NULL, NULL);
