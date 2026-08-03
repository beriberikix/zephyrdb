/*
 * Copyright (c) 2026 ZephyrDB contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `zdb kv` commands. Compiled only when CONFIG_ZDB_KV is set; the model gate
 * lives in CMakeLists.txt so this file needs no preprocessor conditionals.
 */

#include "zephyrdb_shell.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/printk.h>

/*
 * How a shell value maps onto storage. A bare token is ambiguous — "0xff" is
 * either a four-character string or one byte, and guessing silently corrupts
 * data — so the operator names the encoding and "zdb kv set <TAB>" teaches
 * them the choices.
 */
enum zdb_shell_kv_enc {
	ZDB_SHELL_KV_ENC_STR, /* text plus its NUL terminator */
	ZDB_SHELL_KV_ENC_RAW, /* text without a terminator */
	ZDB_SHELL_KV_ENC_HEX, /* decoded hex digits */
};

/* argv: [0]=encoding keyword [1]=namespace [2]=key [3]=value */
static int zdb_shell_kv_set(const struct shell *sh, char **argv, enum zdb_shell_kv_enc enc)
{
	zdb_t *db;
	zdb_kv_t kv;
	zdb_status_t st;
	size_t len = 0U;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	if (enc == ZDB_SHELL_KV_ENC_HEX) {
		rc = zdb_shell_parse_hex(argv[3], zdb_shell_scratch, sizeof(zdb_shell_scratch),
					 &len);
		if (rc == -ENOSPC) {
			shell_error(sh, "error: value exceeds %u bytes",
				    (unsigned int)sizeof(zdb_shell_scratch));
			goto out;
		}
		if (rc != 0) {
			rc = zdb_shell_bad_arg(sh, "hex value", argv[3]);
			goto out;
		}
	}

	st = zdb_kv_open(db, argv[1], &kv);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv open", st);
		goto out;
	}

	switch (enc) {
	case ZDB_SHELL_KV_ENC_STR:
		st = zdb_kv_set_str(&kv, argv[2], argv[3]);
		len = strlen(argv[3]) + 1U;
		break;
	case ZDB_SHELL_KV_ENC_RAW:
		len = strlen(argv[3]);
		st = zdb_kv_set(&kv, argv[2], argv[3], len);
		break;
	case ZDB_SHELL_KV_ENC_HEX:
	default:
		st = zdb_kv_set(&kv, argv[2], zdb_shell_scratch, len);
		break;
	}

	(void)zdb_kv_close(&kv);

	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv set", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "namespace: %s", argv[1]);
	shell_print(sh, "key: %s", argv[2]);
	shell_print(sh, "bytes: %u", (unsigned int)len);

out:
	zdb_shell_unlock();
	return rc;
}

#define ZDB_SHELL_KV_SET_CMD(_kw, _enc)                                                            \
	static int cmd_zdb_kv_set_##_kw(const struct shell *sh, size_t argc, char **argv)           \
	{                                                                                          \
		ARG_UNUSED(argc);                                                                  \
		return zdb_shell_kv_set(sh, argv, _enc);                                           \
	}

ZDB_SHELL_KV_SET_CMD(str, ZDB_SHELL_KV_ENC_STR)
ZDB_SHELL_KV_SET_CMD(raw, ZDB_SHELL_KV_ENC_RAW)
ZDB_SHELL_KV_SET_CMD(hex, ZDB_SHELL_KV_ENC_HEX)

static int cmd_zdb_kv_get(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_kv_t kv;
	zdb_status_t st;
	size_t out_len = 0U;
	size_t shown;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_kv_open(db, argv[1], &kv);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv open", st);
		goto out;
	}

	st = zdb_kv_get(&kv, argv[2], zdb_shell_scratch, sizeof(zdb_shell_scratch), &out_len);
	(void)zdb_kv_close(&kv);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv get", st);
		goto out;
	}

	/*
	 * The buffer is sized to one KV I/O slab block, which is the largest a
	 * record can be, so this reports "no" for anything actually storable.
	 */
	shown = MIN(out_len, sizeof(zdb_shell_scratch));

	shell_print(sh, "namespace: %s", argv[1]);
	shell_print(sh, "key: %s", argv[2]);
	shell_print(sh, "len: %u", (unsigned int)out_len);
	shell_print(sh, "truncated: %s", (shown < out_len) ? "yes" : "no");

	/* Always hex, so the value round-trips through "zdb kv set hex". */
	zdb_shell_print_hex(sh, "hex", zdb_shell_scratch, shown);

	if (zdb_shell_printable(zdb_shell_scratch, shown)) {
		size_t text_len = shown;

		if (zdb_shell_scratch[text_len - 1U] == 0x00) {
			text_len--;
		}
		shell_print(sh, "str: %.*s", (int)text_len, (const char *)zdb_shell_scratch);
	}

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_kv_delete(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_kv_t kv;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_kv_open(db, argv[1], &kv);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv open", st);
		goto out;
	}

	st = zdb_kv_delete(&kv, argv[2]);
	(void)zdb_kv_close(&kv);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv delete", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "namespace: %s", argv[1]);
	shell_print(sh, "key: %s", argv[2]);

out:
	zdb_shell_unlock();
	return rc;
}

/* Render up to ZDB_SHELL_PREVIEW_BYTES as hex, marking a clipped tail. */
static void zdb_shell_kv_preview(char *out, size_t capacity, const uint8_t *data, size_t len,
				 size_t total)
{
	size_t n = MIN(len, (size_t)ZDB_SHELL_PREVIEW_BYTES);
	size_t off = 0U;

	for (size_t i = 0U; (i < n) && ((off + 3U) < capacity); i++) {
		(void)snprintk(&out[off], 3U, "%02x", data[i]);
		off += 2U;
	}
	out[off] = '\0';

	if ((total > n) && ((off + 3U) < capacity)) {
		(void)strcat(out, "..");
	}
}

static int cmd_zdb_kv_list(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_kv_t kv;
	zdb_kv_iter_t iter;
	zdb_status_t st;
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	uint8_t value[ZDB_SHELL_PREVIEW_BYTES];
	char preview[(ZDB_SHELL_PREVIEW_BYTES * 2U) + 3U];
	uint64_t limit = ZDB_SHELL_LIST_DEFAULT;
	size_t key_len = 0U;
	size_t value_len = 0U;
	uint32_t shown = 0U;
	bool more = false;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	rc = zdb_shell_parse_listing_args(sh, argc, argv, 2U, &limit, NULL, NULL, NULL);
	if (rc != 0) {
		goto out;
	}

	st = zdb_kv_open(db, argv[1], &kv);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv open", st);
		goto out;
	}

	st = zdb_kv_iter_open(&kv, &iter);
	if (st != ZDB_OK) {
		(void)zdb_kv_close(&kv);
		rc = zdb_shell_fail(sh, "kv iter open", st);
		goto out;
	}

	shell_print(sh, "namespace: %s", argv[1]);

	while (true) {
		st = zdb_kv_iter_next(&iter, key, sizeof(key), &key_len, value, sizeof(value),
				      &value_len);
		if (st == ZDB_ERR_NOT_FOUND) {
			break;
		}
		if (st != ZDB_OK) {
			rc = zdb_shell_fail(sh, "kv iter next", st);
			break;
		}

		if ((uint64_t)shown >= limit) {
			/* One more entry exists, so the listing really is short. */
			more = true;
			break;
		}

		zdb_shell_kv_preview(preview, sizeof(preview), value,
				     MIN(value_len, sizeof(value)), value_len);
		shell_print(sh, "[%u] key=%s len=%u hex=%s", shown, key, (unsigned int)value_len,
			    preview);
		shown++;
	}

	(void)zdb_kv_iter_close(&iter);
	(void)zdb_kv_close(&kv);

	shell_print(sh, "shown: %u", shown);
	shell_print(sh, "truncated: %s", more ? "yes" : "no");

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_kv_reset(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_kv_t kv;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_kv_open(db, argv[1], &kv);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv open", st);
		goto out;
	}

	st = zdb_kv_reset_namespace(&kv);
	(void)zdb_kv_close(&kv);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv reset", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "namespace: %s", argv[1]);

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_kv_defaults(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_kv_defaults_apply(db);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "kv defaults", st);
		goto out;
	}

	shell_print(sh, "status: ok");

out:
	zdb_shell_unlock();
	return rc;
}

#define ZDB_HELP_KV      SHELL_HELP("Key-value commands.", NULL)
#define ZDB_HELP_KV_GET  SHELL_HELP("Read a value, as hex and as text when printable.",           \
				    "<namespace> <key>")
#define ZDB_HELP_KV_LIST SHELL_HELP("List a namespace's keys with a value preview.",              \
				    "<namespace> [limit]\n"                                      \
				    "Values are previewed; use \"zdb kv get\" for the whole one.")
#define ZDB_HELP_KV_DEL  SHELL_HELP("Delete a key.", "<namespace> <key>")
#define ZDB_HELP_KV_SET  SHELL_HELP("Store a value under a named encoding.", NULL)
#define ZDB_HELP_KV_SET_STR                                                                        \
	SHELL_HELP("Store text together with its NUL terminator.", "<namespace> <key> <text>")
#define ZDB_HELP_KV_SET_RAW                                                                        \
	SHELL_HELP("Store text without a NUL terminator.", "<namespace> <key> <text>")
#define ZDB_HELP_KV_SET_HEX                                                                        \
	SHELL_HELP("Store raw bytes decoded from hex digits.",                                     \
		   "<namespace> <key> <hexdigits>\n"                                               \
		   "An even number of digits, optionally 0x-prefixed.")
#define ZDB_HELP_KV_RESET                                                                          \
	SHELL_HELP("Delete every key in a namespace, then re-apply its defaults.", "<namespace>")
#define ZDB_HELP_KV_DEFAULTS                                                                       \
	SHELL_HELP("Write any missing entries of the configured defaults table.", NULL)

SHELL_SUBCMD_SET_CREATE(zdb_kv_set_cmds, (zdb, kv, set));
SHELL_SUBCMD_ADD((zdb, kv, set), str, NULL, ZDB_HELP_KV_SET_STR, cmd_zdb_kv_set_str, 4, 0);
SHELL_SUBCMD_ADD((zdb, kv, set), raw, NULL, ZDB_HELP_KV_SET_RAW, cmd_zdb_kv_set_raw, 4, 0);
SHELL_SUBCMD_ADD((zdb, kv, set), hex, NULL, ZDB_HELP_KV_SET_HEX, cmd_zdb_kv_set_hex, 4, 0);

SHELL_SUBCMD_SET_CREATE(zdb_kv_cmds, (zdb, kv));
SHELL_SUBCMD_ADD((zdb, kv), get, NULL, ZDB_HELP_KV_GET, cmd_zdb_kv_get, 3, 0);
SHELL_SUBCMD_ADD((zdb, kv), list, NULL, ZDB_HELP_KV_LIST, cmd_zdb_kv_list, 2, 1);
SHELL_SUBCMD_ADD((zdb, kv), delete, NULL, ZDB_HELP_KV_DEL, cmd_zdb_kv_delete, 3, 0);
SHELL_SUBCMD_ADD((zdb, kv), set, &zdb_kv_set_cmds, ZDB_HELP_KV_SET, NULL, 1, 0);
SHELL_SUBCMD_ADD((zdb, kv), reset, NULL, ZDB_HELP_KV_RESET, cmd_zdb_kv_reset, 2, 0);
SHELL_SUBCMD_ADD((zdb, kv), defaults, NULL, ZDB_HELP_KV_DEFAULTS, cmd_zdb_kv_defaults, 1, 0);

ZDB_SHELL_CMD_ADD(kv, &zdb_kv_cmds, ZDB_HELP_KV, NULL, 1, 0);
