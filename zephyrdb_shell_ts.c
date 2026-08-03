/*
 * Copyright (c) 2026 ZephyrDB contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `zdb ts` commands. Compiled only when CONFIG_ZDB_TS is set; the model gate
 * lives in CMakeLists.txt so this file needs no preprocessor conditionals.
 *
 * Every command opens its stream, does its work, and closes again before
 * returning. CONFIG_ZDB_TS_MAX_STREAMS defaults to 1, so a stream left open
 * would make every other command here report BUSY.
 */

#include "zephyrdb_shell.h"

#include <errno.h>
#include <string.h>

static int zdb_shell_parse_agg(const char *s, zdb_ts_agg_t *agg)
{
	if ((s == NULL) || (agg == NULL)) {
		return -EINVAL;
	}

	if (strcmp(s, "min") == 0) {
		*agg = ZDB_TS_AGG_MIN;
	} else if (strcmp(s, "max") == 0) {
		*agg = ZDB_TS_AGG_MAX;
	} else if (strcmp(s, "avg") == 0) {
		*agg = ZDB_TS_AGG_AVG;
	} else if (strcmp(s, "sum") == 0) {
		*agg = ZDB_TS_AGG_SUM;
	} else if (strcmp(s, "count") == 0) {
		*agg = ZDB_TS_AGG_COUNT;
	} else {
		return -EINVAL;
	}

	return 0;
}

static const char *zdb_shell_agg_str(zdb_ts_agg_t agg)
{
	switch (agg) {
	case ZDB_TS_AGG_MIN:
		return "min";
	case ZDB_TS_AGG_MAX:
		return "max";
	case ZDB_TS_AGG_AVG:
		return "avg";
	case ZDB_TS_AGG_SUM:
		return "sum";
	case ZDB_TS_AGG_COUNT:
		return "count";
	default:
		return "unknown";
	}
}

static int cmd_zdb_ts_append(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	if (zdb_shell_parse_u64(argv[2], &sample.ts_ms) != 0) {
		rc = zdb_shell_bad_arg(sh, "ts_ms", argv[2]);
		goto out;
	}
	if (zdb_shell_parse_i64(argv[3], &sample.value) != 0) {
		rc = zdb_shell_bad_arg(sh, "value", argv[3]);
		goto out;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = zdb_ts_append_i64(&ts, &sample);

	/*
	 * No explicit flush: closing a stream writes out whatever is buffered,
	 * so forcing one here would only add a timeout that can fail.
	 */
	(void)zdb_ts_close(&ts);

	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts append", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "stream: %s", argv[1]);
	shell_print(sh, "ts_ms: %llu", (unsigned long long)sample.ts_ms);
	shell_print(sh, "value: %lld", (long long)sample.value);

out:
	zdb_shell_unlock();
	return rc;
}

/* argv: [0]="read"|"tail" [1]=stream [2]=limit [3]=from_ms [4]=to_ms */
static int zdb_shell_ts_walk(const struct shell *sh, size_t argc, char **argv, bool descending)
{
	zdb_t *db;
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_ts_window_t window = ZDB_TS_WINDOW_ALL;
	zdb_ts_sample_i64_t sample;
	zdb_status_t st;
	uint64_t limit = ZDB_SHELL_LIST_DEFAULT;
	uint32_t shown = 0U;
	bool more = false;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	rc = zdb_shell_parse_listing_args(sh, argc, argv, 2U, &limit, &window.from_ts_ms,
					  &window.to_ts_ms, NULL);
	if (rc != 0) {
		goto out;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = descending ? zdb_ts_cursor_open_desc(&ts, window, NULL, NULL, &cursor)
			: zdb_ts_cursor_open(&ts, window, NULL, NULL, &cursor);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts cursor open", st);
		if (st == ZDB_ERR_UNSUPPORTED && descending) {
			shell_print(sh, "hint: this backend cannot walk backwards; "
					"use \"zdb ts read\"");
		}
		(void)zdb_ts_close(&ts);
		goto out;
	}

	shell_print(sh, "stream: %s", argv[1]);
	shell_print(sh, "order: %s", descending ? "desc" : "asc");

	while (true) {
		if ((uint64_t)shown >= limit) {
			/*
			 * Ask for one more only to learn whether any remain, so
			 * the truncated line states a fact instead of a guess.
			 */
			more = (zdb_ts_cursor_next_sample(&cursor, &sample) == ZDB_OK);
			break;
		}

		st = zdb_ts_cursor_next_sample(&cursor, &sample);
		if (st == ZDB_ERR_NOT_FOUND) {
			break;
		}
		if (st != ZDB_OK) {
			rc = zdb_shell_fail(sh, "ts cursor next", st);
			break;
		}

		shell_print(sh, "[%u] ts_ms=%llu value=%lld", shown,
			    (unsigned long long)sample.ts_ms, (long long)sample.value);
		shown++;
	}

	(void)zdb_cursor_close(&cursor);
	(void)zdb_ts_close(&ts);

	shell_print(sh, "shown: %u", shown);
	shell_print(sh, "truncated: %s", more ? "yes" : "no");

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_ts_read(const struct shell *sh, size_t argc, char **argv)
{
	return zdb_shell_ts_walk(sh, argc, argv, false);
}

static int cmd_zdb_ts_tail(const struct shell *sh, size_t argc, char **argv)
{
	return zdb_shell_ts_walk(sh, argc, argv, true);
}

static int cmd_zdb_ts_agg(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_t ts;
	zdb_ts_window_t window = ZDB_TS_WINDOW_ALL;
	zdb_ts_agg_t agg;
	zdb_ts_agg_result_t result;
	zdb_status_t st;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	if (zdb_shell_parse_agg(argv[2], &agg) != 0) {
		shell_error(sh, "error: unknown aggregate: %s (use min|max|avg|sum|count)", argv[2]);
		rc = -EINVAL;
		goto out;
	}

	rc = zdb_shell_parse_listing_args(sh, argc, argv, 3U, NULL, &window.from_ts_ms,
					  &window.to_ts_ms, NULL);
	if (rc != 0) {
		goto out;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = zdb_ts_query_aggregate(&ts, window, agg, &result);
	(void)zdb_ts_close(&ts);

	/*
	 * "Nothing matched that window" is an answer, not a failure: the
	 * value-bearing aggregates report NOT_FOUND for an empty match, and the
	 * shell absorbs it rather than making the operator parse an error.
	 */
	if (st == ZDB_ERR_NOT_FOUND) {
		shell_print(sh, "stream: %s", argv[1]);
		shell_print(sh, "agg: %s", zdb_shell_agg_str(agg));
		shell_print(sh, "points: 0");
		shell_print(sh, "matched: none");
		goto out;
	}
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts agg", st);
		goto out;
	}

	shell_print(sh, "stream: %s", argv[1]);
	shell_print(sh, "agg: %s", zdb_shell_agg_str(result.agg));
	shell_print(sh, "points: %u", result.points);
	shell_print(sh, "value: %f", result.value);
	shell_print(sh, "truncated: %s", result.truncated ? "yes" : "no");

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_ts_flush_sync(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_t ts;
	k_timeout_t timeout = K_SECONDS(2);
	zdb_status_t st;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	if ((argc > 2U) && (zdb_shell_parse_timeout(argv[2], &timeout) != 0)) {
		rc = zdb_shell_bad_arg(sh, "timeout_ms", argv[2]);
		goto out;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = zdb_ts_flush_sync(&ts, timeout);
	(void)zdb_ts_close(&ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts flush sync", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "stream: %s", argv[1]);

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_ts_flush_async(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_t ts;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = zdb_ts_flush_async(&ts);
	(void)zdb_ts_close(&ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts flush async", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "stream: %s", argv[1]);

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_ts_recover(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_t ts;
	zdb_status_t st;
	size_t truncated = 0U;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = zdb_ts_recover_stream(&ts, &truncated);
	(void)zdb_ts_close(&ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts recover", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "stream: %s", argv[1]);
	shell_print(sh, "truncated_bytes: %u", (unsigned int)truncated);

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_ts_watermark_get(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_t ts;
	zdb_status_t st;
	uint64_t consumed = 0U;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = zdb_ts_watermark_get(&ts, &consumed);
	(void)zdb_ts_close(&ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts watermark get", st);
		if (st == ZDB_ERR_NOT_FOUND) {
			shell_print(sh, "hint: no watermark stored for this stream");
		}
		goto out;
	}

	shell_print(sh, "stream: %s", argv[1]);
	shell_print(sh, "consumed_ts_ms: %llu", (unsigned long long)consumed);

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_ts_watermark_set(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_t ts;
	zdb_status_t st;
	uint64_t consumed;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	if (zdb_shell_parse_u64(argv[2], &consumed) != 0) {
		rc = zdb_shell_bad_arg(sh, "ts_ms", argv[2]);
		goto out;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = zdb_ts_watermark_set(&ts, consumed);
	(void)zdb_ts_close(&ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts watermark set", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "stream: %s", argv[1]);
	shell_print(sh, "consumed_ts_ms: %llu", (unsigned long long)consumed);

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_ts_watermark_clear(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_ts_t ts;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_ts_open(db, argv[1], &ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts open", st);
		goto out;
	}

	st = zdb_ts_watermark_clear(&ts);
	(void)zdb_ts_close(&ts);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "ts watermark clear", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "stream: %s", argv[1]);

out:
	zdb_shell_unlock();
	return rc;
}

#define ZDB_HELP_TS SHELL_HELP("Time-series commands.", NULL)
#define ZDB_HELP_TS_APPEND                                                                         \
	SHELL_HELP("Append one sample; it is persisted when the stream closes.",                    \
		   "<stream> <ts_ms> <value>")
#define ZDB_HELP_TS_READ                                                                           \
	SHELL_HELP("Walk samples oldest-first.",                                                   \
		   "<stream> [limit] [from_ms to_ms]\n"                                            \
		   "limit defaults to 20; the window bounds are inclusive.")
#define ZDB_HELP_TS_TAIL                                                                           \
	SHELL_HELP("Walk samples newest-first.",                                                   \
		   "<stream> [limit] [from_ms to_ms]\n"                                            \
		   "Unsupported on the FCB backend, which only walks forward.")
#define ZDB_HELP_TS_AGG                                                                            \
	SHELL_HELP("Aggregate samples over a window.",                                             \
		   "<stream> min|max|avg|sum|count [from_ms to_ms]")
#define ZDB_HELP_TS_FLUSH SHELL_HELP("Persist buffered samples.", NULL)
#define ZDB_HELP_TS_FLUSH_SYNC                                                                     \
	SHELL_HELP("Flush and wait.",                                                              \
		   "<stream> [timeout_ms]\n"                                                       \
		   "Defaults to 2000; \"forever\" waits indefinitely. 0 does not wait at all,\n"    \
		   "so it reports BUSY unless the flush completes immediately.")
#define ZDB_HELP_TS_FLUSH_ASYNC                                                                    \
	SHELL_HELP("Queue a flush on the work queue.",                                             \
		   "<stream>\n"                                                                    \
		   "The stream closes when the command returns, and closing also writes out\n"     \
		   "buffered samples, so this exercises the async path rather than deferring\n"    \
		   "the work.")
#define ZDB_HELP_TS_RECOVER                                                                        \
	SHELL_HELP("Scan the stream and truncate a corrupt tail.", "<stream>")
#define ZDB_HELP_TS_WMK       SHELL_HELP("Consumer watermark.", NULL)
#define ZDB_HELP_TS_WMK_GET   SHELL_HELP("Read the consumed watermark.", "<stream>")
#define ZDB_HELP_TS_WMK_SET                                                                        \
	SHELL_HELP("Record how far a consumer has processed.", "<stream> <ts_ms>")
#define ZDB_HELP_TS_WMK_CLEAR SHELL_HELP("Forget the consumed watermark.", "<stream>")

SHELL_SUBCMD_SET_CREATE(zdb_ts_flush_cmds, (zdb, ts, flush));
SHELL_SUBCMD_ADD((zdb, ts, flush), sync, NULL, ZDB_HELP_TS_FLUSH_SYNC, cmd_zdb_ts_flush_sync, 2, 1);
SHELL_SUBCMD_ADD((zdb, ts, flush), async, NULL, ZDB_HELP_TS_FLUSH_ASYNC, cmd_zdb_ts_flush_async, 2,
		 0);

SHELL_SUBCMD_SET_CREATE(zdb_ts_wmk_cmds, (zdb, ts, watermark));
SHELL_SUBCMD_ADD((zdb, ts, watermark), get, NULL, ZDB_HELP_TS_WMK_GET, cmd_zdb_ts_watermark_get, 2, 0);
SHELL_SUBCMD_ADD((zdb, ts, watermark), set, NULL, ZDB_HELP_TS_WMK_SET, cmd_zdb_ts_watermark_set, 3, 0);
SHELL_SUBCMD_ADD((zdb, ts, watermark), clear, NULL, ZDB_HELP_TS_WMK_CLEAR, cmd_zdb_ts_watermark_clear, 2,
		 0);

SHELL_SUBCMD_SET_CREATE(zdb_ts_cmds, (zdb, ts));
SHELL_SUBCMD_ADD((zdb, ts), append, NULL, ZDB_HELP_TS_APPEND, cmd_zdb_ts_append, 4, 0);
SHELL_SUBCMD_ADD((zdb, ts), read, NULL, ZDB_HELP_TS_READ, cmd_zdb_ts_read, 2, 3);
SHELL_SUBCMD_ADD((zdb, ts), tail, NULL, ZDB_HELP_TS_TAIL, cmd_zdb_ts_tail, 2, 3);
SHELL_SUBCMD_ADD((zdb, ts), agg, NULL, ZDB_HELP_TS_AGG, cmd_zdb_ts_agg, 3, 2);
SHELL_SUBCMD_ADD((zdb, ts), flush, &zdb_ts_flush_cmds, ZDB_HELP_TS_FLUSH, NULL, 1, 0);
SHELL_SUBCMD_ADD((zdb, ts), recover, NULL, ZDB_HELP_TS_RECOVER, cmd_zdb_ts_recover, 2, 0);
SHELL_SUBCMD_ADD((zdb, ts), watermark, &zdb_ts_wmk_cmds, ZDB_HELP_TS_WMK, NULL, 1, 0);

ZDB_SHELL_CMD_ADD(ts, &zdb_ts_cmds, ZDB_HELP_TS, NULL, 1, 0);
