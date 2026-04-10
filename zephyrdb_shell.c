#include "zephyrdb.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

static zdb_t *g_shell_db;

void zdb_shell_register(zdb_t *db)
{
	g_shell_db = db;
}

static zdb_t *zdb_shell_db_get(const struct shell *sh)
{
	if (g_shell_db == NULL) {
		shell_error(sh, "ZephyrDB not registered. Call zdb_shell_register() after zdb_init().");
		return NULL;
	}

	return g_shell_db;
}

static const char *zdb_health_str(zdb_health_t health)
{
	switch (health) {
	case ZDB_HEALTH_OK:
		return "OK";
	case ZDB_HEALTH_DEGRADED:
		return "DEGRADED";
	case ZDB_HEALTH_READONLY:
		return "READONLY";
	case ZDB_HEALTH_FAULT:
		return "FAULT";
	default:
		return "UNKNOWN";
	}
}

static int cmd_zdb_health(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zdb_t *db = zdb_shell_db_get(sh);
	zdb_health_t health;

	if (db == NULL) {
		return -ENODEV;
	}

	health = zdb_health(db);
	shell_print(sh, "health: %s", zdb_health_str(health));

	return 0;
}

static int cmd_zdb_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zdb_t *db = zdb_shell_db_get(sh);
	zdb_ts_stats_t stats;

	if (db == NULL) {
		return -ENODEV;
	}

	zdb_ts_stats_get(db, &stats);
	shell_print(sh, "recover_runs: %u", stats.recover_runs);
	shell_print(sh, "recover_failures: %u", stats.recover_failures);
	shell_print(sh, "recover_truncated_bytes: %llu",
		    (unsigned long long)stats.recover_truncated_bytes);
	shell_print(sh, "crc_failures: %u", stats.crc_failures);
	shell_print(sh, "corrupt_records: %u", stats.corrupt_records);
	shell_print(sh, "unsupported_versions: %u", stats.unsupported_versions);

	return 0;
}

#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
static int cmd_zdb_kv_set(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db = zdb_shell_db_get(sh);
	zdb_kv_t kv;
	zdb_status_t status;

	if (db == NULL) {
		return -ENODEV;
	}

	ARG_UNUSED(argc);
	status = zdb_kv_open(db, argv[1], &kv);
	if (status != ZDB_OK) {
		shell_error(sh, "kv open failed: %s", zdb_status_str(status));
		return -EIO;
	}

	status = zdb_kv_set(&kv, argv[2], argv[3], strlen(argv[3]));
	(void)zdb_kv_close(&kv);
	if (status != ZDB_OK) {
		shell_error(sh, "kv set failed: %s", zdb_status_str(status));
		return -EIO;
	}

	shell_print(sh, "kv set ok");
	return 0;
}

static int cmd_zdb_kv_get(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db = zdb_shell_db_get(sh);
	zdb_kv_t kv;
	zdb_status_t status;
	uint8_t value[256];
	size_t out_len = 0U;

	if (db == NULL) {
		return -ENODEV;
	}

	ARG_UNUSED(argc);
	status = zdb_kv_open(db, argv[1], &kv);
	if (status != ZDB_OK) {
		shell_error(sh, "kv open failed: %s", zdb_status_str(status));
		return -EIO;
	}

	status = zdb_kv_get(&kv, argv[2], value, sizeof(value), &out_len);
	(void)zdb_kv_close(&kv);
	if (status != ZDB_OK) {
		shell_error(sh, "kv get failed: %s", zdb_status_str(status));
		return -EIO;
	}

	shell_print(sh, "len: %u", (unsigned int)out_len);
	shell_hexdump(sh, value, out_len);

	return 0;
}

static int cmd_zdb_kv_delete(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db = zdb_shell_db_get(sh);
	zdb_kv_t kv;
	zdb_status_t status;

	if (db == NULL) {
		return -ENODEV;
	}

	ARG_UNUSED(argc);
	status = zdb_kv_open(db, argv[1], &kv);
	if (status != ZDB_OK) {
		shell_error(sh, "kv open failed: %s", zdb_status_str(status));
		return -EIO;
	}

	status = zdb_kv_delete(&kv, argv[2]);
	(void)zdb_kv_close(&kv);
	if (status != ZDB_OK) {
		shell_error(sh, "kv delete failed: %s", zdb_status_str(status));
		return -EIO;
	}

	shell_print(sh, "kv delete ok");
	return 0;
}
#endif

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
static int parse_u64(const char *s, uint64_t *out)
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

static int parse_i64(const char *s, int64_t *out)
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

static int cmd_zdb_ts_append(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db = zdb_shell_db_get(sh);
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample;
	zdb_status_t status;

	if (db == NULL) {
		return -ENODEV;
	}

	ARG_UNUSED(argc);
	if (parse_u64(argv[2], &sample.ts_ms) != 0) {
		shell_error(sh, "invalid ts_ms: %s", argv[2]);
		return -EINVAL;
	}
	if (parse_i64(argv[3], &sample.value) != 0) {
		shell_error(sh, "invalid value: %s", argv[3]);
		return -EINVAL;
	}

	status = zdb_ts_open(db, argv[1], &ts);
	if (status != ZDB_OK) {
		shell_error(sh, "ts open failed: %s", zdb_status_str(status));
		return -EIO;
	}

	status = zdb_ts_append_i64(&ts, &sample);
	if (status == ZDB_OK) {
		status = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	}
	(void)zdb_ts_close(&ts);

	if (status != ZDB_OK) {
		shell_error(sh, "ts append failed: %s", zdb_status_str(status));
		return -EIO;
	}

	shell_print(sh, "ts append ok");
	return 0;
}

static int cmd_zdb_ts_query(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db = zdb_shell_db_get(sh);
	zdb_ts_t ts;
	zdb_ts_window_t window = ZDB_TS_WINDOW_ALL;
	zdb_ts_agg_t agg;
	zdb_ts_agg_result_t result;
	zdb_status_t status;

	if (db == NULL) {
		return -ENODEV;
	}

	if (zdb_shell_parse_agg(argv[2], &agg) != 0) {
		shell_error(sh, "invalid agg: %s (use: min|max|avg|sum|count)", argv[2]);
		return -EINVAL;
	}

	if (argc == 5U) {
		if (parse_u64(argv[3], &window.from_ts_ms) != 0) {
			shell_error(sh, "invalid from_ms: %s", argv[3]);
			return -EINVAL;
		}
		if (parse_u64(argv[4], &window.to_ts_ms) != 0) {
			shell_error(sh, "invalid to_ms: %s", argv[4]);
			return -EINVAL;
		}
	} else if (argc != 3U) {
		shell_error(sh, "usage: zdb ts query <stream> <agg> [from_ms to_ms]");
		return -EINVAL;
	}

	status = zdb_ts_open(db, argv[1], &ts);
	if (status != ZDB_OK) {
		shell_error(sh, "ts open failed: %s", zdb_status_str(status));
		return -EIO;
	}

	status = zdb_ts_query_aggregate(&ts, window, agg, &result);
	(void)zdb_ts_close(&ts);
	if (status != ZDB_OK) {
		shell_error(sh, "ts query failed: %s", zdb_status_str(status));
		return -EIO;
	}

	shell_print(sh, "agg: %s", argv[2]);
	shell_print(sh, "points: %u", result.points);
	shell_print(sh, "value: %f", result.value);

	return 0;
}

static int cmd_zdb_ts_flush(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db = zdb_shell_db_get(sh);
	zdb_ts_t ts;
	zdb_status_t status;

	if (db == NULL) {
		return -ENODEV;
	}

	ARG_UNUSED(argc);
	status = zdb_ts_open(db, argv[1], &ts);
	if (status != ZDB_OK) {
		shell_error(sh, "ts open failed: %s", zdb_status_str(status));
		return -EIO;
	}

	status = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	(void)zdb_ts_close(&ts);
	if (status != ZDB_OK) {
		shell_error(sh, "ts flush failed: %s", zdb_status_str(status));
		return -EIO;
	}

	shell_print(sh, "ts flush ok");
	return 0;
}
#endif

#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
static int cmd_zdb_doc_open(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db = zdb_shell_db_get(sh);
	zdb_doc_t doc;
	zdb_status_t status;

	if (db == NULL) {
		return -ENODEV;
	}

	ARG_UNUSED(argc);
	status = zdb_doc_open(db, argv[1], argv[2], &doc);
	if (status != ZDB_OK) {
		shell_error(sh, "doc open failed: %s", zdb_status_str(status));
		return -EIO;
	}

	(void)zdb_doc_close(&doc);
	shell_print(sh, "doc open ok");

	return 0;
}
#endif

#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zdb_kv,
	SHELL_CMD_ARG(set, NULL, "Set KV value: <namespace> <key> <value>", cmd_zdb_kv_set, 4, 0),
	SHELL_CMD_ARG(get, NULL, "Get KV value: <namespace> <key>", cmd_zdb_kv_get, 3, 0),
	SHELL_CMD_ARG(delete, NULL, "Delete KV key: <namespace> <key>", cmd_zdb_kv_delete, 3, 0),
	SHELL_SUBCMD_SET_END
);
#endif

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zdb_ts,
	SHELL_CMD_ARG(append, NULL, "Append sample: <stream> <ts_ms> <value>", cmd_zdb_ts_append, 4, 0),
	SHELL_CMD_ARG(query, NULL, "Aggregate query: <stream> <agg> [from_ms to_ms]", cmd_zdb_ts_query,
		      3, 2),
	SHELL_CMD_ARG(flush, NULL, "Flush stream: <stream>", cmd_zdb_ts_flush, 2, 0),
	SHELL_SUBCMD_SET_END
);
#endif

#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zdb_doc,
	SHELL_CMD_ARG(open, NULL, "Open document: <collection> <doc_id>", cmd_zdb_doc_open, 3, 0),
	SHELL_SUBCMD_SET_END
);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zdb,
	SHELL_CMD(health, NULL, "Show DB health", cmd_zdb_health),
	SHELL_CMD(stats, NULL, "Show TS stats", cmd_zdb_stats),
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
	SHELL_CMD(kv, &sub_zdb_kv, "KV commands", NULL),
#endif
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	SHELL_CMD(ts, &sub_zdb_ts, "Time-series commands", NULL),
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
	SHELL_CMD(doc, &sub_zdb_doc, "Document commands", NULL),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(zdb, &sub_zdb, "ZephyrDB commands", NULL);