/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drives the `zdb` command tree through the dummy shell backend and asserts on
 * what each command prints. Covers the plumbing (every leaf reachable), the
 * status-to-errno mapping, and the behaviour of all three models.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_dummy.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/zms.h>
#include <zephyr/storage/flash_map.h>
#include <errno.h>
#include <string.h>

#include "zephyrdb.h"

static struct zms_fs g_zms;
static bool g_zms_mounted;
static const struct shell *g_sh;
static char g_out[CONFIG_SHELL_BACKEND_DUMMY_BUF_SIZE];

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

/*
 * shell_backend_dummy_get_output() resets the read pointer, so it is called
 * exactly once per command and the result copied before anything else runs.
 */
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

#define zassert_out_lacks(_needle)                                                                 \
	zassert_is_null(strstr(g_out, (_needle)), "unexpected \"%s\" in output:\n%s", (_needle),    \
			g_out)

static void zms_backend_mount_once(void)
{
	struct flash_pages_info info;
	int rc;

	if (g_zms_mounted) {
		return;
	}

	g_zms.flash_device = PARTITION_DEVICE(storage_partition);
	zassert_true(device_is_ready(g_zms.flash_device), "storage device not ready");

	g_zms.offset = PARTITION_OFFSET(storage_partition);
	rc = flash_get_page_info_by_offs(g_zms.flash_device, g_zms.offset, &info);
	zassert_equal(rc, 0, "flash page info failed: %d", rc);

	g_zms.sector_size = info.size;
	g_zms.sector_count = 3U;

	rc = zms_mount(&g_zms);
	zassert_equal(rc, 0, "zms mount failed: %d", rc);
	g_zms_mounted = true;
}

static void *shell_suite_setup(void)
{
	g_sh = shell_backend_dummy_get_ptr();
	zassert_not_null(g_sh, "dummy shell backend missing");
	WAIT_FOR(shell_ready(g_sh), 20000, k_msleep(1));
	zassert_true(shell_ready(g_sh), "dummy shell backend never became ready");

	return NULL;
}

static void shell_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zms_backend_mount_once();
	g_cfg.kv_backend_fs = &g_zms;
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
	zdb_shell_register(&g_db);
}

static void shell_after(void *fixture)
{
	ARG_UNUSED(fixture);
	zdb_shell_register(NULL);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(zdb_shell, NULL, shell_suite_setup, shell_before, shell_after, NULL);

/* ---------------------------------------------------------------- plumbing */

/*
 * Every leaf reachable. A subtree whose section tag collided with another
 * would drop out of its parent's list here rather than in a later assertion.
 */
ZTEST(zdb_shell, test_every_leaf_is_reachable)
{
	static const char *const leaves[] = {
		"zdb health",
		"zdb info",
		"zdb stats show",
		"zdb stats reset",
		"zdb stats export",
		"zdb kv get",
		"zdb kv list",
		"zdb kv delete",
		"zdb kv set str",
		"zdb kv set raw",
		"zdb kv set hex",
		"zdb kv reset",
		"zdb kv defaults",
		"zdb ts append",
		"zdb ts read",
		"zdb ts tail",
		"zdb ts agg",
		"zdb ts flush sync",
		"zdb ts flush async",
		"zdb ts recover",
		"zdb ts watermark get",
		"zdb ts watermark set",
		"zdb ts watermark clear",
		"zdb doc create",
		"zdb doc delete",
		"zdb doc get",
		"zdb doc list",
		"zdb doc set i64",
		"zdb doc set f64",
		"zdb doc set str",
		"zdb doc set bool",
		"zdb doc set bytes",
		"zdb doc find i64",
		"zdb doc find f64",
		"zdb doc find str",
		"zdb doc find bool",
	};
	char cmd[64];

	for (size_t i = 0U; i < ARRAY_SIZE(leaves); i++) {
		int rc;

		(void)snprintk(cmd, sizeof(cmd), "%s -h", leaves[i]);
		rc = sh_exec(cmd);
		zassert_equal(rc, SHELL_CMD_HELP_PRINTED, "\"%s\" not reachable (rc=%d):\n%s", cmd,
			      rc, g_out);
	}
}

ZTEST(zdb_shell, test_unknown_command_rejected)
{
	zassert_not_equal(sh_exec("zdb bogus"), 0, "unknown subcommand accepted");
}

ZTEST(zdb_shell, test_wrong_argc_rejected)
{
	zassert_not_equal(sh_exec("zdb kv get onearg"), 0, "missing argument accepted");
}

ZTEST(zdb_shell, test_unregistered_instance_reports_enodev)
{
	zdb_shell_register(NULL);
	zassert_equal(sh_exec("zdb health"), -ENODEV, "expected -ENODEV:\n%s", g_out);
	zassert_out_has("not registered");
	zdb_shell_register(&g_db);
}

/*
 * The shell's status-to-errno map must agree with the library's inverse, or a
 * caller reading errno would draw a different conclusion than the printed
 * status. Checked through the commands that can produce each status.
 */
ZTEST(zdb_shell, test_status_maps_to_errno)
{
	zassert_equal(sh_exec("zdb kv get nosuchns nosuchkey"), -ENOENT, "NOT_FOUND -> -ENOENT:\n%s",
		      g_out);
	zassert_out_has("NOT_FOUND");

	zassert_equal(sh_exec("zdb kv set hex t k zz"), -EINVAL, "bad hex -> -EINVAL:\n%s", g_out);
}

/* -------------------------------------------------------------------- core */

ZTEST(zdb_shell, test_health_and_info)
{
	zassert_equal(sh_exec("zdb health"), 0, "health failed:\n%s", g_out);
	zassert_out_has("health: OK");

	zassert_equal(sh_exec("zdb info"), 0, "info failed:\n%s", g_out);
	zassert_out_has("version: " ZDB_VERSION_STRING);
	zassert_out_has("kv_backend: zms");
	zassert_out_has("ts_backend: littlefs");
	zassert_out_has("doc: yes");
}

ZTEST(zdb_shell, test_stats_show_reset_export)
{
	zassert_equal(sh_exec("zdb stats show"), 0, "stats show failed:\n%s", g_out);
	zassert_out_has("recover_runs:");
	zassert_out_has("crc_failures:");
	zassert_out_has("unsupported_versions:");

	zassert_equal(sh_exec("zdb stats reset"), 0, "stats reset failed:\n%s", g_out);
	zassert_out_has("status: ok");

	zassert_equal(sh_exec("zdb stats show"), 0, "stats show failed:\n%s", g_out);
	zassert_out_has("recover_runs: 0");

	zassert_equal(sh_exec("zdb stats export"), 0, "stats export failed:\n%s", g_out);
	zassert_out_has("hex: ");
	zassert_out_has("valid: yes");
}

/* ---------------------------------------------------------------------- kv */

/*
 * Regression for the pre-0.6 shell, which stored strlen() bytes and so dropped
 * the terminator that zdb_kv_set_str() promises.
 */
ZTEST(zdb_shell, test_kv_set_str_stores_terminator)
{
	zassert_equal(sh_exec("zdb kv set str t_str k hello"), 0, "set str failed:\n%s", g_out);
	zassert_out_has("status: ok");

	zassert_equal(sh_exec("zdb kv get t_str k"), 0, "get failed:\n%s", g_out);
	zassert_out_has("len: 6");
	zassert_out_has("hex: 68656c6c6f00");
	zassert_out_has("str: hello");
}

ZTEST(zdb_shell, test_kv_set_raw_omits_terminator)
{
	zassert_equal(sh_exec("zdb kv set raw t_raw k hello"), 0, "set raw failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb kv get t_raw k"), 0, "get failed:\n%s", g_out);
	zassert_out_has("len: 5");
	zassert_out_has("hex: 68656c6c6f");
}

ZTEST(zdb_shell, test_kv_hex_round_trip)
{
	zassert_equal(sh_exec("zdb kv set hex t_hex k 00ff10"), 0, "set hex failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb kv get t_hex k"), 0, "get failed:\n%s", g_out);
	zassert_out_has("len: 3");
	zassert_out_has("hex: 00ff10");
	/* Not printable, so no text rendering is offered. */
	zassert_out_lacks("str: ");

	/* The printed hex is what "set hex" accepts, so the value round-trips. */
	zassert_equal(sh_exec("zdb kv set hex t_hex k2 00ff10"), 0, "re-set failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb kv get t_hex k2"), 0, "get failed:\n%s", g_out);
	zassert_out_has("hex: 00ff10");
}

ZTEST(zdb_shell, test_kv_bad_hex_rejected_and_nothing_written)
{
	/* An odd digit count is rejected rather than silently zero-padded. */
	zassert_equal(sh_exec("zdb kv set hex t_bad k 0f0"), -EINVAL, "odd digits accepted:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb kv set hex t_bad k zz"), -EINVAL, "non-hex accepted:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb kv get t_bad k"), -ENOENT, "key should not exist:\n%s", g_out);
}

/*
 * Regression for the pre-0.6 shell's fixed 256-byte read buffer. The buffer is
 * now sized to one KV I/O slab block, which bounds any storable record, so a
 * value at the largest storable size must report itself complete.
 */
ZTEST(zdb_shell, test_kv_get_does_not_truncate_at_the_slab_bound)
{
	char cmd[CONFIG_SHELL_CMD_BUFF_SIZE];
	size_t overhead = 3U + strlen("t_max") + strlen("k");
	size_t value_bytes = CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE - overhead;
	size_t digits;
	size_t off;

	/* Keep the hex payload inside the command line the shell can hold. */
	value_bytes = MIN(value_bytes, (size_t)(CONFIG_SHELL_CMD_BUFF_SIZE / 2U) - 32U);
	digits = value_bytes * 2U;

	off = (size_t)snprintk(cmd, sizeof(cmd), "zdb kv set hex t_max k ");
	zassert_true((off + digits + 1U) < sizeof(cmd), "command line too short for the test");
	for (size_t i = 0U; i < digits; i++) {
		cmd[off + i] = 'a';
	}
	cmd[off + digits] = '\0';

	zassert_equal(sh_exec(cmd), 0, "set of %u bytes failed:\n%s", (unsigned int)value_bytes,
		      g_out);

	zassert_equal(sh_exec("zdb kv get t_max k"), 0, "get failed:\n%s", g_out);
	zassert_out_has("truncated: no");
}

ZTEST(zdb_shell, test_kv_list_reports_entries)
{
	zassert_equal(sh_exec("zdb kv set str t_list a one"), 0, "set failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb kv set str t_list b two"), 0, "set failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb kv list t_list"), 0, "list failed:\n%s", g_out);
	zassert_out_has("key=a");
	zassert_out_has("key=b");
	zassert_out_has("shown: 2");
	zassert_out_has("truncated: no");

	/* A limit shorter than the namespace reports itself honestly. */
	zassert_equal(sh_exec("zdb kv list t_list 1"), 0, "limited list failed:\n%s", g_out);
	zassert_out_has("shown: 1");
	zassert_out_has("truncated: yes");
}

ZTEST(zdb_shell, test_kv_delete_and_reset)
{
	zassert_equal(sh_exec("zdb kv set str t_del k v"), 0, "set failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb kv delete t_del k"), 0, "delete failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb kv get t_del k"), -ENOENT, "key survived delete:\n%s", g_out);

	zassert_equal(sh_exec("zdb kv set str t_reset k v"), 0, "set failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb kv reset t_reset"), 0, "reset failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb kv list t_reset"), 0, "list failed:\n%s", g_out);
	zassert_out_has("shown: 0");

	zassert_equal(sh_exec("zdb kv defaults"), 0, "defaults failed:\n%s", g_out);
	zassert_out_has("status: ok");
}

/* ---------------------------------------------------------------------- ts */

/*
 * Regression for the pre-0.6 "ts append", which forced a 2 s flush. Closing a
 * stream already persists, so the sample must be readable without one.
 */
ZTEST(zdb_shell, test_ts_append_persists_without_an_explicit_flush)
{
	zassert_equal(sh_exec("zdb ts append s_one 1000 42"), 0, "append failed:\n%s", g_out);
	zassert_out_has("status: ok");

	zassert_equal(sh_exec("zdb ts read s_one"), 0, "read failed:\n%s", g_out);
	zassert_out_has("ts_ms=1000 value=42");
	zassert_out_has("shown: 1");
}

ZTEST(zdb_shell, test_ts_read_tail_and_window)
{
	zassert_equal(sh_exec("zdb ts append s_walk 1000 20"), 0, "append failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts append s_walk 1001 21"), 0, "append failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts append s_walk 1002 22"), 0, "append failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb ts read s_walk"), 0, "read failed:\n%s", g_out);
	zassert_out_has("order: asc");
	zassert_out_has("[0] ts_ms=1000 value=20");
	zassert_out_has("shown: 3");
	zassert_out_has("truncated: no");

	zassert_equal(sh_exec("zdb ts read s_walk 2"), 0, "limited read failed:\n%s", g_out);
	zassert_out_has("shown: 2");
	zassert_out_has("truncated: yes");

	zassert_equal(sh_exec("zdb ts tail s_walk"), 0, "tail failed:\n%s", g_out);
	zassert_out_has("order: desc");
	zassert_out_has("[0] ts_ms=1002 value=22");

	zassert_equal(sh_exec("zdb ts read s_walk 100 1000 1001"), 0, "window read failed:\n%s",
		      g_out);
	zassert_out_has("shown: 2");
}

/* The one arity rule SHELL_CMD_ARG counts cannot express. */
ZTEST(zdb_shell, test_ts_half_window_rejected)
{
	zassert_equal(sh_exec("zdb ts read s_walk 10 1000"), -EINVAL, "half window accepted:\n%s",
		      g_out);
	zassert_out_has("given together");
}

/* Regression: the pre-0.6 "ts query" never reported result.truncated. */
ZTEST(zdb_shell, test_ts_agg_reports_truncation_and_empty_windows)
{
	zassert_equal(sh_exec("zdb ts append s_agg 1000 10"), 0, "append failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts append s_agg 1001 20"), 0, "append failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb ts agg s_agg avg"), 0, "agg failed:\n%s", g_out);
	zassert_out_has("agg: avg");
	zassert_out_has("points: 2");
	zassert_out_has("truncated: ");

	/* An empty window is an answer, not a failure. */
	zassert_equal(sh_exec("zdb ts agg s_agg count 9000 9999"), 0, "empty window errored:\n%s",
		      g_out);
	zassert_out_has("points: 0");

	zassert_equal(sh_exec("zdb ts agg s_agg bogus"), -EINVAL, "bad aggregate accepted:\n%s",
		      g_out);
	zassert_out_has("min|max|avg|sum|count");
}

ZTEST(zdb_shell, test_ts_flush_recover_and_watermark)
{
	int rc;

	zassert_equal(sh_exec("zdb ts append s_wm 1000 1"), 0, "append failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb ts flush sync s_wm"), 0, "flush sync failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts flush async s_wm"), 0, "flush async failed:\n%s", g_out);

	/*
	 * A zero timeout means "do not wait", so BUSY is a correct answer as
	 * well as success; what matters is that it parses and reports one.
	 */
	rc = sh_exec("zdb ts flush sync s_wm 0");
	zassert_true((rc == 0) || (rc == -EBUSY), "flush sync 0 gave %d:\n%s", rc, g_out);

	zassert_equal(sh_exec("zdb ts recover s_wm"), 0, "recover failed:\n%s", g_out);
	zassert_out_has("truncated_bytes: 0");

	zassert_equal(sh_exec("zdb ts watermark get s_wm"), -ENOENT, "expected no watermark:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb ts watermark set s_wm 1500"), 0, "watermark set failed:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb ts watermark get s_wm"), 0, "watermark get failed:\n%s", g_out);
	zassert_out_has("consumed_ts_ms: 1500");
	zassert_equal(sh_exec("zdb ts watermark clear s_wm"), 0, "watermark clear failed:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb ts watermark get s_wm"), -ENOENT, "watermark survived clear:\n%s",
		      g_out);
}

/*
 * With CONFIG_ZDB_TS_MAX_STREAMS at its default of 1, a handler that forgets to
 * close its stream makes the next command on a different stream report BUSY.
 */
ZTEST(zdb_shell, test_every_ts_command_releases_its_stream)
{
	zassert_equal(sh_exec("zdb ts append s_a 1000 1"), 0, "append a failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts read s_a"), 0, "read a failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts agg s_a count"), 0, "agg a failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts watermark set s_a 1"), 0, "watermark a failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb ts append s_b 2000 2"), 0, "append b failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts read s_b"), 0, "read b failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts tail s_b"), 0, "tail b failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb ts flush sync s_b"), 0, "flush b failed:\n%s", g_out);
}

/* --------------------------------------------------------------------- doc */

ZTEST(zdb_shell, test_doc_create_refuses_to_overwrite)
{
	zassert_equal(sh_exec("zdb doc create c_new d1"), 0, "create failed:\n%s", g_out);
	zassert_out_has("status: ok");

	zassert_not_equal(sh_exec("zdb doc create c_new d1"), 0, "second create overwrote:\n%s",
			  g_out);
	zassert_out_has("already exists");
}

ZTEST(zdb_shell, test_doc_set_and_get_every_type)
{
	zassert_equal(sh_exec("zdb doc create c_typ d1"), 0, "create failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb doc set i64 c_typ d1 temp 21"), 0, "set i64 failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb doc set f64 c_typ d1 cal 21.5"), 0, "set f64 failed:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb doc set str c_typ d1 label kitchen"), 0, "set str failed:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb doc set bool c_typ d1 active yes"), 0, "set bool failed:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb doc set bytes c_typ d1 raw 00ff10"), 0, "set bytes failed:\n%s",
		      g_out);

	/* No type argument: each stored field reports its own. */
	zassert_equal(sh_exec("zdb doc get c_typ d1"), 0, "get failed:\n%s", g_out);
	zassert_out_has("field_count: 5");
	zassert_out_has("temp i64 21");
	zassert_out_has("cal f64 21.5");
	zassert_out_has("label str kitchen");
	zassert_out_has("active bool true");
	zassert_out_has("raw bytes: 00ff10");
	zassert_out_has("shown: 5");

	zassert_equal(sh_exec("zdb doc get c_typ d1 label"), 0, "single field get failed:\n%s",
		      g_out);
	zassert_out_has("label str kitchen");

	zassert_equal(sh_exec("zdb doc get c_typ d1 nosuch"), -ENOENT, "missing field accepted:\n%s",
		      g_out);
}

ZTEST(zdb_shell, test_doc_set_rejects_bad_values)
{
	zassert_equal(sh_exec("zdb doc create c_bad d1"), 0, "create failed:\n%s", g_out);

	zassert_equal(sh_exec("zdb doc set bool c_bad d1 f maybe"), -EINVAL, "bad bool accepted:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb doc set bytes c_bad d1 f 0f0"), -EINVAL, "odd hex accepted:\n%s",
		      g_out);
	zassert_equal(sh_exec("zdb doc set i64 c_bad d1 f notanumber"), -EINVAL,
		      "bad integer accepted:\n%s", g_out);

	/* Setting a field on a document that does not exist points at create. */
	zassert_not_equal(sh_exec("zdb doc set i64 c_bad nosuch f 1"), 0,
			  "set on missing document succeeded:\n%s", g_out);
	zassert_out_has("zdb doc create");
}

ZTEST(zdb_shell, test_doc_list_and_find)
{
	zassert_equal(sh_exec("zdb doc create c_find d1"), 0, "create failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb doc set i64 c_find d1 temp 21"), 0, "set failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb doc set str c_find d1 label kitchen"), 0, "set failed:\n%s",
		      g_out);

	zassert_equal(sh_exec("zdb doc list"), 0, "list failed:\n%s", g_out);
	zassert_out_has("document=d1");

	zassert_equal(sh_exec("zdb doc find i64 temp 21"), 0, "find i64 failed:\n%s", g_out);
	zassert_out_has("document=d1");

	zassert_equal(sh_exec("zdb doc find str label kitchen"), 0, "find str failed:\n%s", g_out);
	zassert_out_has("document=d1");

	zassert_equal(sh_exec("zdb doc find i64 temp 999"), 0, "no-match find errored:\n%s", g_out);
	zassert_out_has("shown: 0");
}

/* A "doc set" that did not save would lose the field on reopen. */
ZTEST(zdb_shell, test_doc_set_saves_to_storage)
{
	zassert_equal(sh_exec("zdb doc create c_save d1"), 0, "create failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb doc set i64 c_save d1 temp 7"), 0, "set failed:\n%s", g_out);

	(void)zdb_deinit(&g_db);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "re-init failed");
	zdb_shell_register(&g_db);

	zassert_equal(sh_exec("zdb doc get c_save d1 temp"), 0, "field lost across restart:\n%s",
		      g_out);
	zassert_out_has("temp i64 7");
}

/*
 * Query results are heap-backed. A path that forgets zdb_doc_metadata_free()
 * exhausts the pool long before this loop ends.
 */
ZTEST(zdb_shell, test_doc_query_releases_its_results)
{
	zassert_equal(sh_exec("zdb doc create c_leak d1"), 0, "create failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb doc set str c_leak d1 label x"), 0, "set failed:\n%s", g_out);

	for (unsigned int i = 0U; i < 200U; i++) {
		zassert_equal(sh_exec("zdb doc list"), 0, "list failed on iteration %u:\n%s", i,
			      g_out);
	}

	for (unsigned int i = 0U; i < 200U; i++) {
		zassert_equal(sh_exec("zdb doc find str label x"), 0, "find failed on iteration %u:\n%s",
			      i, g_out);
	}
}

ZTEST(zdb_shell, test_doc_delete)
{
	zassert_equal(sh_exec("zdb doc create c_del d1"), 0, "create failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb doc delete c_del d1"), 0, "delete failed:\n%s", g_out);
	zassert_equal(sh_exec("zdb doc get c_del d1"), -ENOENT, "document survived delete:\n%s",
		      g_out);
}
