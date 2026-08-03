/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FCB time-series backend writes through and keeps no sidecar state, so
 * part of the `zdb ts` surface cannot be served on it, and the shell has to
 * report that rather than claim success.
 *
 * This suite is build_only (see testcase.yaml): FCB needs a real flash area,
 * which native_sim's partition layout does not provide. Building it is still
 * worth doing — it is the only configuration in the repository that compiles
 * the FCB code paths at all, and they had gone stale before it existed. Run
 * the assertions below on a board with an FCB flash area to exercise them.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <errno.h>
#include <string.h>

#include "zephyrdb.h"

static const struct shell *g_sh;
static char g_out[CONFIG_SHELL_BACKEND_DUMMY_BUF_SIZE];

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static int sh_exec(const char *cmd)
{
	const char *buf;
	size_t size = 0U;
	size_t copy;
	int rc;

	shell_backend_dummy_clear_output(g_sh);
	rc = shell_execute_cmd(g_sh, cmd);

	buf = shell_backend_dummy_get_output(g_sh, &size);
	copy = MIN(size, sizeof(g_out) - 1U);
	(void)memcpy(g_out, buf, copy);
	g_out[copy] = '\0';

	return rc;
}

#define zassert_out_has(_needle)                                                                   \
	zassert_not_null(strstr(g_out, (_needle)), "missing \"%s\" in output:\n%s", (_needle),      \
			 g_out)

static void *fcb_suite_setup(void)
{
	g_sh = shell_backend_dummy_get_ptr();
	zassert_not_null(g_sh, "dummy shell backend missing");
	WAIT_FOR(shell_ready(g_sh), 20000, k_msleep(1));
	zassert_true(shell_ready(g_sh), "dummy shell backend never became ready");

	return NULL;
}

static void fcb_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
	zdb_shell_register(&g_db);
}

static void fcb_after(void *fixture)
{
	ARG_UNUSED(fixture);
	zdb_shell_register(NULL);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(zdb_shell_fcb, NULL, fcb_suite_setup, fcb_before, fcb_after, NULL);

ZTEST(zdb_shell_fcb, test_info_names_the_backend)
{
	zassert_equal(sh_exec("zdb info"), 0, "info failed:\n%s", g_out);
	zassert_out_has("ts_backend: fcb");
	/* The models that are off contribute no subtree. */
	zassert_out_has("kv: no");
	zassert_out_has("doc: no");
}

ZTEST(zdb_shell_fcb, test_append_and_read_are_supported)
{
	zassert_equal(sh_exec("zdb ts append f_s 1000 42"), 0, "append failed:\n%s", g_out);
	zassert_out_has("status: ok");

	zassert_equal(sh_exec("zdb ts read f_s"), 0, "read failed:\n%s", g_out);
	zassert_out_has("order: asc");
}

/* Flushing is a no-op here, which must read as success, not as an error. */
ZTEST(zdb_shell_fcb, test_flush_is_accepted_as_a_no_op)
{
	zassert_equal(sh_exec("zdb ts append f_f 1000 1"), 0, "append failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts flush sync f_f"), 0, "flush sync errored:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts flush async f_f"), 0, "flush async errored:\n%s", g_out);
}

ZTEST(zdb_shell_fcb, test_unsupported_commands_say_so)
{
	zassert_equal(sh_exec("zdb ts append f_u 1000 1"), 0, "append failed:\n%s", g_out);

	/* Backwards traversal: FCB can only walk forward. */
	zassert_equal(sh_exec("zdb ts tail f_u"), -ENOTSUP, "tail should be unsupported:\n%s",
		      g_out);
	zassert_out_has("UNSUPPORTED");
	zassert_out_has("hint:");

	zassert_equal(sh_exec("zdb ts agg f_u avg"), -ENOTSUP, "agg should be unsupported:\n%s",
		      g_out);
	zassert_out_has("UNSUPPORTED");

	/* Watermarks live in a sidecar the backend does not keep. */
	zassert_equal(sh_exec("zdb ts watermark get f_u"), -ENOTSUP,
		      "watermark get should be unsupported:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts watermark set f_u 5"), -ENOTSUP,
		      "watermark set should be unsupported:\n%s", g_out);
}
