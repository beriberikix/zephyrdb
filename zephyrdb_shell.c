/*
 * Copyright (c) 2026 ZephyrDB contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Root of the `zdb` command tree, the core commands, and the parsing and
 * formatting helpers the per-model shell sources share. Command reference:
 * docs/shell.md.
 */

#include "zephyrdb_shell.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/printk.h>

/*
 * The scratch buffer and the bound instance are shared across every command,
 * and shell_execute_cmd() can be driven from any thread with several backends
 * active at once. One mutex around the whole surface makes it single-threaded
 * by construction.
 */
static K_MUTEX_DEFINE(zdb_shell_mtx);
static zdb_t *zdb_shell_db;

uint8_t zdb_shell_scratch[ZDB_SHELL_SCRATCH_SIZE];

void zdb_shell_register(zdb_t *db)
{
	(void)k_mutex_lock(&zdb_shell_mtx, K_FOREVER);
	zdb_shell_db = db;
	(void)k_mutex_unlock(&zdb_shell_mtx);
}

int zdb_shell_lock(const struct shell *sh, zdb_t **out_db)
{
	(void)k_mutex_lock(&zdb_shell_mtx, K_FOREVER);

	if (zdb_shell_db == NULL) {
		(void)k_mutex_unlock(&zdb_shell_mtx);
		shell_error(sh, "error: ZephyrDB not registered; call zdb_shell_register() "
				"after zdb_init()");
		return -ENODEV;
	}

	*out_db = zdb_shell_db;
	return 0;
}

void zdb_shell_unlock(void)
{
	(void)k_mutex_unlock(&zdb_shell_mtx);
}

/*
 * The inverse of zdb_status_from_errno() in zephyrdb_core.c, extended for the
 * two statuses that call has no errno preimage for.
 */
int zdb_shell_errno(zdb_status_t status)
{
	switch (status) {
	case ZDB_OK:
		return 0;
	case ZDB_ERR_INVAL:
		return -EINVAL;
	case ZDB_ERR_NOMEM:
		return -ENOMEM;
	case ZDB_ERR_NOT_FOUND:
		return -ENOENT;
	case ZDB_ERR_BUSY:
		return -EBUSY;
	case ZDB_ERR_TIMEOUT:
		return -ETIMEDOUT;
	case ZDB_ERR_UNSUPPORTED:
		return -ENOTSUP;
	case ZDB_ERR_CORRUPT:
		return -EBADMSG;
	case ZDB_ERR_COLLISION:
		return -EEXIST;
	case ZDB_ERR_IO:
	case ZDB_ERR_INTERNAL:
	default:
		return -EIO;
	}
}

int zdb_shell_fail(const struct shell *sh, const char *op, zdb_status_t status)
{
	shell_error(sh, "error: %s failed: %s", op, zdb_status_str(status));

	switch (status) {
	case ZDB_ERR_UNSUPPORTED:
		shell_print(sh, "hint: not supported by the active backend (see \"zdb info\")");
		break;
	case ZDB_ERR_BUSY:
		shell_print(sh, "hint: another handle is still open; "
				"see CONFIG_ZDB_TS_MAX_STREAMS");
		break;
	case ZDB_ERR_NOMEM:
		shell_print(sh, "hint: a slab or the heap is exhausted, or the value "
				"exceeds its bound (see \"zdb info\")");
		break;
	default:
		break;
	}

	return zdb_shell_errno(status);
}

int zdb_shell_bad_arg(const struct shell *sh, const char *what, const char *got)
{
	shell_error(sh, "error: invalid %s: %s", what, (got != NULL) ? got : "");
	return -EINVAL;
}

int zdb_shell_parse_u64(const char *s, uint64_t *out)
{
	char *end = NULL;
	unsigned long long v;

	if ((s == NULL) || (out == NULL)) {
		return -EINVAL;
	}

	errno = 0;
	v = strtoull(s, &end, 0);
	if ((errno != 0) || (end == s) || (*end != '\0')) {
		return -EINVAL;
	}

	*out = (uint64_t)v;
	return 0;
}

int zdb_shell_parse_i64(const char *s, int64_t *out)
{
	char *end = NULL;
	long long v;

	if ((s == NULL) || (out == NULL)) {
		return -EINVAL;
	}

	errno = 0;
	v = strtoll(s, &end, 0);
	if ((errno != 0) || (end == s) || (*end != '\0')) {
		return -EINVAL;
	}

	*out = (int64_t)v;
	return 0;
}

int zdb_shell_parse_f64(const char *s, double *out)
{
	char *end = NULL;
	double v;

	if ((s == NULL) || (out == NULL)) {
		return -EINVAL;
	}

	errno = 0;
	v = strtod(s, &end);
	if ((errno != 0) || (end == s) || (*end != '\0')) {
		return -EINVAL;
	}

	*out = v;
	return 0;
}

int zdb_shell_parse_bool(const char *s, bool *out)
{
	static const char *const yes[] = {"1", "true", "yes", "on"};
	static const char *const no[] = {"0", "false", "no", "off"};

	if ((s == NULL) || (out == NULL)) {
		return -EINVAL;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(yes); i++) {
		if (strcmp(s, yes[i]) == 0) {
			*out = true;
			return 0;
		}
	}

	for (size_t i = 0U; i < ARRAY_SIZE(no); i++) {
		if (strcmp(s, no[i]) == 0) {
			*out = false;
			return 0;
		}
	}

	/* Anything else is rejected rather than quietly reading as false. */
	return -EINVAL;
}

static int zdb_shell_hex_nibble(char c)
{
	if ((c >= '0') && (c <= '9')) {
		return c - '0';
	}
	if ((c >= 'a') && (c <= 'f')) {
		return (c - 'a') + 10;
	}
	if ((c >= 'A') && (c <= 'F')) {
		return (c - 'A') + 10;
	}

	return -1;
}

/*
 * Decode "deadbeef" or "0xdeadbeef". An odd digit count is rejected rather
 * than zero-padded: padding changes the value the operator typed and there is
 * no way to know which end they meant.
 */
int zdb_shell_parse_hex(const char *s, uint8_t *out, size_t capacity, size_t *out_len)
{
	size_t digits;

	if ((s == NULL) || (out == NULL) || (out_len == NULL)) {
		return -EINVAL;
	}

	if ((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X'))) {
		s += 2;
	}

	digits = strlen(s);
	if ((digits == 0U) || ((digits % 2U) != 0U)) {
		return -EINVAL;
	}
	if ((digits / 2U) > capacity) {
		return -ENOSPC;
	}

	for (size_t i = 0U; i < digits; i += 2U) {
		int hi = zdb_shell_hex_nibble(s[i]);
		int lo = zdb_shell_hex_nibble(s[i + 1U]);

		if ((hi < 0) || (lo < 0)) {
			return -EINVAL;
		}

		out[i / 2U] = (uint8_t)(((uint32_t)hi << 4) | (uint32_t)lo);
	}

	*out_len = digits / 2U;
	return 0;
}

int zdb_shell_parse_timeout(const char *s, k_timeout_t *out)
{
	uint64_t ms;

	if ((s == NULL) || (out == NULL)) {
		return -EINVAL;
	}

	if (strcmp(s, "forever") == 0) {
		*out = K_FOREVER;
		return 0;
	}

	if (zdb_shell_parse_u64(s, &ms) != 0) {
		return -EINVAL;
	}

	*out = (ms == 0U) ? K_NO_WAIT : K_MSEC(ms);
	return 0;
}

bool zdb_shell_printable(const uint8_t *data, size_t len)
{
	if ((data == NULL) || (len == 0U)) {
		return false;
	}

	/* A value written by zdb_kv_set_str() carries its terminator. */
	if (data[len - 1U] == 0x00) {
		len--;
		if (len == 0U) {
			return false;
		}
	}

	for (size_t i = 0U; i < len; i++) {
		if ((data[i] < 0x20U) || (data[i] > 0x7eU)) {
			return false;
		}
	}

	return true;
}

/* 16 bytes is 32 characters, which keeps a line well inside the print buffer. */
#define ZDB_SHELL_HEX_PER_LINE 16U

void zdb_shell_print_hex(const struct shell *sh, const char *label, const uint8_t *data, size_t len)
{
	char line[(ZDB_SHELL_HEX_PER_LINE * 2U) + 1U];
	size_t off = 0U;

	if ((data == NULL) || (len == 0U)) {
		shell_print(sh, "%s:", label);
		return;
	}

	while (off < len) {
		size_t n = MIN(len - off, (size_t)ZDB_SHELL_HEX_PER_LINE);

		for (size_t i = 0U; i < n; i++) {
			(void)snprintk(&line[i * 2U], 3U, "%02x", data[off + i]);
		}
		line[n * 2U] = '\0';

		/* Repeat the label so every line of a long value greps alone. */
		shell_print(sh, "%s: %s", label, line);
		off += n;
	}
}

int zdb_shell_parse_listing_args(const struct shell *sh, size_t argc, char **argv, size_t first,
				 uint64_t *limit, uint64_t *from_ms, uint64_t *to_ms,
				 bool *windowed)
{
	size_t extra = argc - first;
	size_t window_first = first;

	if (windowed != NULL) {
		*windowed = false;
	}

	/* A NULL limit means this command takes only the window pair. */
	if (limit != NULL) {
		if (extra >= 1U) {
			if (zdb_shell_parse_u64(argv[first], limit) != 0) {
				return zdb_shell_bad_arg(sh, "limit", argv[first]);
			}
			extra--;
		}
		window_first = first + 1U;
	}

	if (extra == 0U) {
		return 0;
	}

	/*
	 * SHELL_CMD_ARG optional counts cannot express "these two go together",
	 * so this is the one arity rule the handlers check by hand.
	 */
	if (extra != 2U) {
		shell_error(sh, "error: from_ms and to_ms must be given together");
		return -EINVAL;
	}

	if ((from_ms != NULL) && (zdb_shell_parse_u64(argv[window_first], from_ms) != 0)) {
		return zdb_shell_bad_arg(sh, "from_ms", argv[window_first]);
	}
	if ((to_ms != NULL) && (zdb_shell_parse_u64(argv[window_first + 1U], to_ms) != 0)) {
		return zdb_shell_bad_arg(sh, "to_ms", argv[window_first + 1U]);
	}

	if (windowed != NULL) {
		*windowed = true;
	}

	return 0;
}

static int cmd_zdb_health(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	shell_print(sh, "health: %s", zdb_health_str(zdb_health(db)));

	zdb_shell_unlock();
	return 0;
}

static const char *zdb_shell_kv_backend(void)
{
	if (IS_ENABLED(CONFIG_ZDB_KV_BACKEND_ZMS)) {
		return "zms";
	}
	if (IS_ENABLED(CONFIG_ZDB_KV_BACKEND_NVS)) {
		return "nvs";
	}

	return "none";
}

static const char *zdb_shell_ts_backend(void)
{
	if (IS_ENABLED(CONFIG_ZDB_TS_BACKEND_FCB)) {
		return "fcb";
	}
	if (IS_ENABLED(CONFIG_ZDB_TS_BACKEND_LITTLEFS)) {
		return "littlefs";
	}

	return "none";
}

/*
 * There is no API to enumerate namespaces, streams, or collections, so tab
 * completion cannot offer them. This command is the substitute: it reports what
 * the build can do and the bounds an operator will hit.
 */
static int cmd_zdb_info(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	shell_print(sh, "version: %s", ZDB_VERSION_STRING);
	shell_print(sh, "health: %s", zdb_health_str(zdb_health(db)));
	shell_print(sh, "kv: %s", IS_ENABLED(CONFIG_ZDB_KV) ? "yes" : "no");
	shell_print(sh, "ts: %s", IS_ENABLED(CONFIG_ZDB_TS) ? "yes" : "no");
	shell_print(sh, "doc: %s", IS_ENABLED(CONFIG_ZDB_DOC) ? "yes" : "no");
	shell_print(sh, "eventing: %s", IS_ENABLED(CONFIG_ZDB_EVENTING) ? "yes" : "no");
	shell_print(sh, "stats: %s", IS_ENABLED(CONFIG_ZDB_STATS) ? "yes" : "no");
	shell_print(sh, "kv_backend: %s", zdb_shell_kv_backend());
	shell_print(sh, "ts_backend: %s", zdb_shell_ts_backend());
	shell_print(sh, "max_key_len: %d", CONFIG_ZDB_MAX_KEY_LEN);
	shell_print(sh, "kv_value_max: %d", ZDB_SHELL_KV_VALUE_MAX);
	shell_print(sh, "hex_input_max: %d", ZDB_SHELL_HEX_MAX);

	zdb_shell_unlock();
	return 0;
}

static int cmd_zdb_stats_show(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_stats_t stats;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	zdb_ts_stats_get(db, &stats);
	shell_print(sh, "recover_runs: %u", stats.recover_runs);
	shell_print(sh, "recover_failures: %u", stats.recover_failures);
	shell_print(sh, "recover_truncated_bytes: %llu",
		    (unsigned long long)stats.recover_truncated_bytes);
	shell_print(sh, "crc_failures: %u", stats.crc_failures);
	shell_print(sh, "corrupt_records: %u", stats.corrupt_records);
	shell_print(sh, "unsupported_versions: %u", stats.unsupported_versions);

	zdb_shell_unlock();
	return 0;
}

static int cmd_zdb_stats_reset(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	zdb_ts_stats_reset(db);
	shell_print(sh, "status: ok");

	zdb_shell_unlock();
	return 0;
}

/*
 * Prints the exact byte string an operator would paste into a bug report, and
 * validates it on the way out so the report says whether it survived.
 */
static int cmd_zdb_stats_export(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_stats_export_t export_data;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_ts_stats_export(db, &export_data);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "stats export", st);
		goto out;
	}

	shell_print(sh, "version: %u", (unsigned int)export_data.version);
	shell_print(sh, "crc: %08x", (unsigned int)export_data.crc);
	shell_print(sh, "bytes: %u", (unsigned int)sizeof(export_data));
	zdb_shell_print_hex(sh, "hex", (const uint8_t *)&export_data, sizeof(export_data));
	shell_print(sh, "valid: %s",
		    (zdb_ts_stats_export_validate(&export_data) == ZDB_OK) ? "yes" : "no");

out:
	zdb_shell_unlock();
	return rc;
}

#define ZDB_HELP_HEALTH SHELL_HELP("Report instance health.", NULL)
#define ZDB_HELP_INFO   SHELL_HELP("Report enabled models, backends, and compile-time limits.", NULL)
#define ZDB_HELP_STATS  SHELL_HELP("Time-series durability counters.", NULL)
#define ZDB_HELP_STATS_SHOW   SHELL_HELP("Print the durability counters.", NULL)
#define ZDB_HELP_STATS_RESET  SHELL_HELP("Zero the durability counters.", NULL)
#define ZDB_HELP_STATS_EXPORT \
	SHELL_HELP("Print the packed CRC-protected counter export, hex and decoded.", NULL)

SHELL_SUBCMD_SET_CREATE(zdb_stats_cmds, (zdb, stats));
SHELL_SUBCMD_ADD((zdb, stats), show, NULL, ZDB_HELP_STATS_SHOW, cmd_zdb_stats_show, 1, 0);
SHELL_SUBCMD_ADD((zdb, stats), reset, NULL, ZDB_HELP_STATS_RESET, cmd_zdb_stats_reset, 1, 0);
SHELL_SUBCMD_ADD((zdb, stats), export, NULL, ZDB_HELP_STATS_EXPORT, cmd_zdb_stats_export, 1, 0);

ZDB_SHELL_CMD_ADD(health, NULL, ZDB_HELP_HEALTH, cmd_zdb_health, 1, 0);
ZDB_SHELL_CMD_ADD(info, NULL, ZDB_HELP_INFO, cmd_zdb_info, 1, 0);
ZDB_SHELL_CMD_ADD(stats, &zdb_stats_cmds, ZDB_HELP_STATS, NULL, 1, 0);

SHELL_SUBCMD_SET_CREATE(zdb_cmds, (zdb));
SHELL_CMD_REGISTER(zdb, &zdb_cmds, "ZephyrDB commands", NULL);
