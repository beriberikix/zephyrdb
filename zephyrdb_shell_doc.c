/*
 * Copyright (c) 2026 ZephyrDB contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `zdb doc` commands. Compiled only when CONFIG_ZDB_DOC is set; the model gate
 * lives in CMakeLists.txt so this file needs no preprocessor conditionals.
 *
 * "set" names a type because a shell value is untyped text. "get" does not,
 * because the stored field already carries one and the operator usually does
 * not know it.
 */

#include "zephyrdb_shell.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/printk.h>

/*
 * Rows one query reports. Query results are heap-backed and must be released
 * with zdb_doc_metadata_free(), so the array is a fixed compile-time size
 * rather than a Kconfig knob.
 */
#define ZDB_SHELL_DOC_QUERY_MAX 16U

static zdb_doc_metadata_t zdb_shell_doc_results[ZDB_SHELL_DOC_QUERY_MAX];

static const char *zdb_shell_doc_type_str(zdb_doc_field_type_t type)
{
	switch (type) {
	case ZDB_DOC_FIELD_INT64:
		return "i64";
	case ZDB_DOC_FIELD_DOUBLE:
		return "f64";
	case ZDB_DOC_FIELD_STRING:
		return "str";
	case ZDB_DOC_FIELD_BOOL:
		return "bool";
	case ZDB_DOC_FIELD_BYTES:
		return "bytes";
	default:
		return "unsupported";
	}
}

static void zdb_shell_doc_print_field(const struct shell *sh, const zdb_doc_field_t *field)
{
	switch (field->type) {
	case ZDB_DOC_FIELD_INT64:
		shell_print(sh, "%s i64 %lld", field->name, (long long)field->value.i64);
		break;
	case ZDB_DOC_FIELD_DOUBLE:
		shell_print(sh, "%s f64 %f", field->name, field->value.f64);
		break;
	case ZDB_DOC_FIELD_STRING:
		shell_print(sh, "%s str %s", field->name,
			    (field->value.str != NULL) ? field->value.str : "");
		break;
	case ZDB_DOC_FIELD_BOOL:
		shell_print(sh, "%s bool %s", field->name, field->value.b ? "true" : "false");
		break;
	case ZDB_DOC_FIELD_BYTES: {
		/* Borrowed span into the open document; printed before it closes. */
		char label[CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN + sizeof(" bytes")];

		(void)snprintk(label, sizeof(label), "%s bytes", field->name);
		zdb_shell_print_hex(sh, label, field->value.bytes.data, field->value.bytes.len);
		break;
	}
	default:
		shell_print(sh, "%s %s -", field->name, zdb_shell_doc_type_str(field->type));
		break;
	}
}

static int cmd_zdb_doc_create(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_doc_t doc;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	/*
	 * zdb_doc_create() does not check for an existing document, so probing
	 * first is what keeps this command from silently emptying one.
	 */
	st = zdb_doc_open(db, argv[1], argv[2], &doc);
	if (st == ZDB_OK) {
		(void)zdb_doc_close(&doc);
		shell_error(sh, "error: doc create failed: %s", zdb_status_str(ZDB_ERR_COLLISION));
		shell_print(sh, "hint: document already exists; delete it first");
		rc = zdb_shell_errno(ZDB_ERR_COLLISION);
		goto out;
	}

	st = zdb_doc_create(db, argv[1], argv[2], &doc);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "doc create", st);
		goto out;
	}

	st = zdb_doc_save(&doc);
	(void)zdb_doc_close(&doc);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "doc save", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "collection: %s", argv[1]);
	shell_print(sh, "document: %s", argv[2]);

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_doc_delete(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_status_t st;
	int rc;

	ARG_UNUSED(argc);

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_doc_delete(db, argv[1], argv[2]);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "doc delete", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "collection: %s", argv[1]);
	shell_print(sh, "document: %s", argv[2]);

out:
	zdb_shell_unlock();
	return rc;
}

static int cmd_zdb_doc_get(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_doc_t doc;
	zdb_status_t st;
	uint32_t shown = 0U;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_doc_open(db, argv[1], argv[2], &doc);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "doc open", st);
		goto out;
	}

	shell_print(sh, "collection: %s", argv[1]);
	shell_print(sh, "document: %s", argv[2]);

	if (argc > 3U) {
		for (size_t i = 0U; i < doc.field_count; i++) {
			if (strcmp(doc.fields[i].name, argv[3]) == 0) {
				zdb_shell_doc_print_field(sh, &doc.fields[i]);
				shown = 1U;
				break;
			}
		}

		(void)zdb_doc_close(&doc);

		if (shown == 0U) {
			rc = zdb_shell_fail(sh, "doc get", ZDB_ERR_NOT_FOUND);
			goto out;
		}

		goto out;
	}

	shell_print(sh, "created_ms: %llu", (unsigned long long)doc.created_ms);
	shell_print(sh, "updated_ms: %llu", (unsigned long long)doc.updated_ms);
	shell_print(sh, "field_count: %u", (unsigned int)doc.field_count);

	for (size_t i = 0U; i < doc.field_count; i++) {
		zdb_shell_doc_print_field(sh, &doc.fields[i]);
		shown++;
	}

	(void)zdb_doc_close(&doc);
	shell_print(sh, "shown: %u", shown);

out:
	zdb_shell_unlock();
	return rc;
}

/* argv: [0]=type keyword [1]=collection [2]=document [3]=field [4]=value */
static int zdb_shell_doc_set(const struct shell *sh, char **argv, zdb_doc_field_type_t type)
{
	zdb_t *db;
	zdb_doc_t doc;
	zdb_status_t st = ZDB_OK;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	st = zdb_doc_open(db, argv[1], argv[2], &doc);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "doc open", st);
		if (st == ZDB_ERR_NOT_FOUND) {
			shell_print(sh, "hint: create it first with \"zdb doc create %s %s\"",
				    argv[1], argv[2]);
		}
		goto out;
	}

	switch (type) {
	case ZDB_DOC_FIELD_INT64: {
		int64_t value;

		rc = zdb_shell_parse_i64(argv[4], &value);
		if (rc == 0) {
			st = zdb_doc_field_set_i64(&doc, argv[3], value);
		}
		break;
	}
	case ZDB_DOC_FIELD_DOUBLE: {
		double value;

		rc = zdb_shell_parse_f64(argv[4], &value);
		if (rc == 0) {
			st = zdb_doc_field_set_f64(&doc, argv[3], value);
		}
		break;
	}
	case ZDB_DOC_FIELD_STRING:
		st = zdb_doc_field_set_string(&doc, argv[3], argv[4]);
		break;
	case ZDB_DOC_FIELD_BOOL: {
		bool value;

		rc = zdb_shell_parse_bool(argv[4], &value);
		if (rc == 0) {
			st = zdb_doc_field_set_bool(&doc, argv[3], value);
		}
		break;
	}
	case ZDB_DOC_FIELD_BYTES:
	default: {
		size_t len = 0U;

		/* Scratch is held under the same lock as the rest of the command. */
		rc = zdb_shell_parse_hex(argv[4], zdb_shell_scratch, sizeof(zdb_shell_scratch),
					 &len);
		if (rc == 0) {
			st = zdb_doc_field_set_bytes(&doc, argv[3], zdb_shell_scratch, len);
		}
		break;
	}
	}

	if (rc != 0) {
		(void)zdb_doc_close(&doc);
		if (rc == -ENOSPC) {
			shell_error(sh, "error: value exceeds %u bytes",
				    (unsigned int)sizeof(zdb_shell_scratch));
		} else {
			rc = zdb_shell_bad_arg(sh, zdb_shell_doc_type_str(type), argv[4]);
		}
		goto out;
	}

	if (st != ZDB_OK) {
		(void)zdb_doc_close(&doc);
		rc = zdb_shell_fail(sh, "doc field set", st);
		goto out;
	}

	st = zdb_doc_save(&doc);
	(void)zdb_doc_close(&doc);
	if (st != ZDB_OK) {
		rc = zdb_shell_fail(sh, "doc save", st);
		goto out;
	}

	shell_print(sh, "status: ok");
	shell_print(sh, "collection: %s", argv[1]);
	shell_print(sh, "document: %s", argv[2]);
	shell_print(sh, "field: %s", argv[3]);
	shell_print(sh, "type: %s", zdb_shell_doc_type_str(type));

out:
	zdb_shell_unlock();
	return rc;
}

#define ZDB_SHELL_DOC_SET_CMD(_kw, _type)                                                          \
	static int cmd_zdb_doc_set_##_kw(const struct shell *sh, size_t argc, char **argv)          \
	{                                                                                          \
		ARG_UNUSED(argc);                                                                  \
		return zdb_shell_doc_set(sh, argv, _type);                                         \
	}

ZDB_SHELL_DOC_SET_CMD(i64, ZDB_DOC_FIELD_INT64)
ZDB_SHELL_DOC_SET_CMD(f64, ZDB_DOC_FIELD_DOUBLE)
ZDB_SHELL_DOC_SET_CMD(str, ZDB_DOC_FIELD_STRING)
ZDB_SHELL_DOC_SET_CMD(boolean, ZDB_DOC_FIELD_BOOL)
ZDB_SHELL_DOC_SET_CMD(bytes, ZDB_DOC_FIELD_BYTES)

/* Run a prepared query and print its rows; always releases the results. */
static int zdb_shell_doc_report(const struct shell *sh, zdb_t *db, zdb_doc_query_t *query)
{
	zdb_status_t st;
	/* out_count is in/out: it carries the array's capacity into the call. */
	size_t count = ARRAY_SIZE(zdb_shell_doc_results);

	st = zdb_doc_query(db, query, zdb_shell_doc_results, &count);
	if (st != ZDB_OK) {
		return zdb_shell_fail(sh, "doc query", st);
	}

	for (size_t i = 0U; i < count; i++) {
		shell_print(sh, "[%u] collection=%s document=%s fields=%u updated_ms=%llu",
			    (unsigned int)i, zdb_shell_doc_results[i].collection_name,
			    zdb_shell_doc_results[i].document_id,
			    zdb_shell_doc_results[i].field_count,
			    (unsigned long long)zdb_shell_doc_results[i].updated_ms);
	}

	(void)zdb_doc_metadata_free(zdb_shell_doc_results, count);

	shell_print(sh, "shown: %u", (unsigned int)count);
	shell_print(sh, "truncated: %s", (count >= ZDB_SHELL_DOC_QUERY_MAX) ? "yes" : "no");

	return 0;
}

static int cmd_zdb_doc_list(const struct shell *sh, size_t argc, char **argv)
{
	zdb_t *db;
	zdb_doc_query_t query = {0};
	uint64_t limit = ZDB_SHELL_DOC_QUERY_MAX;
	uint64_t from_ms = 0U;
	uint64_t to_ms = 0U;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	rc = zdb_shell_parse_listing_args(sh, argc, argv, 1U, &limit, &from_ms, &to_ms, NULL);
	if (rc != 0) {
		goto out;
	}

	query.from_ms = from_ms;
	query.to_ms = to_ms;
	query.limit = (uint32_t)MIN(limit, (uint64_t)ZDB_SHELL_DOC_QUERY_MAX);

	rc = zdb_shell_doc_report(sh, db, &query);

out:
	zdb_shell_unlock();
	return rc;
}

/* argv: [0]=type keyword [1]=field [2]=value [3]=limit */
static int zdb_shell_doc_find(const struct shell *sh, size_t argc, char **argv,
			      zdb_doc_field_type_t type)
{
	zdb_t *db;
	zdb_doc_query_filter_t filter = {0};
	zdb_doc_query_t query = {0};
	uint64_t limit = ZDB_SHELL_DOC_QUERY_MAX;
	int rc;

	rc = zdb_shell_lock(sh, &db);
	if (rc != 0) {
		return rc;
	}

	if ((argc > 3U) && (zdb_shell_parse_u64(argv[3], &limit) != 0)) {
		rc = zdb_shell_bad_arg(sh, "limit", argv[3]);
		goto out;
	}

	filter.field_name = argv[1];
	filter.type = type;

	switch (type) {
	case ZDB_DOC_FIELD_INT64: {
		int64_t value;

		if (zdb_shell_parse_i64(argv[2], &value) != 0) {
			rc = zdb_shell_bad_arg(sh, "i64", argv[2]);
			goto out;
		}
		filter.numeric_value = (double)value;
		break;
	}
	case ZDB_DOC_FIELD_DOUBLE:
		if (zdb_shell_parse_f64(argv[2], &filter.numeric_value) != 0) {
			rc = zdb_shell_bad_arg(sh, "f64", argv[2]);
			goto out;
		}
		break;
	case ZDB_DOC_FIELD_STRING:
		filter.string_value = argv[2];
		break;
	case ZDB_DOC_FIELD_BOOL:
	default:
		if (zdb_shell_parse_bool(argv[2], &filter.bool_value) != 0) {
			rc = zdb_shell_bad_arg(sh, "bool", argv[2]);
			goto out;
		}
		break;
	}

	query.filters = &filter;
	query.filter_count = 1U;
	query.limit = (uint32_t)MIN(limit, (uint64_t)ZDB_SHELL_DOC_QUERY_MAX);

	rc = zdb_shell_doc_report(sh, db, &query);

out:
	zdb_shell_unlock();
	return rc;
}

#define ZDB_SHELL_DOC_FIND_CMD(_kw, _type)                                                         \
	static int cmd_zdb_doc_find_##_kw(const struct shell *sh, size_t argc, char **argv)         \
	{                                                                                          \
		return zdb_shell_doc_find(sh, argc, argv, _type);                                  \
	}

ZDB_SHELL_DOC_FIND_CMD(i64, ZDB_DOC_FIELD_INT64)
ZDB_SHELL_DOC_FIND_CMD(f64, ZDB_DOC_FIELD_DOUBLE)
ZDB_SHELL_DOC_FIND_CMD(str, ZDB_DOC_FIELD_STRING)
ZDB_SHELL_DOC_FIND_CMD(boolean, ZDB_DOC_FIELD_BOOL)

#define ZDB_HELP_DOC SHELL_HELP("Document commands.", NULL)
#define ZDB_HELP_DOC_CREATE                                                                        \
	SHELL_HELP("Create and save an empty document; refuses to overwrite one.",                  \
		   "<collection> <document_id>")
#define ZDB_HELP_DOC_DELETE SHELL_HELP("Delete a document.", "<collection> <document_id>")
#define ZDB_HELP_DOC_GET                                                                           \
	SHELL_HELP("Print a document's fields, or one named field.",                                \
		   "<collection> <document_id> [field]\n"                                          \
		   "No type is needed: each stored field reports its own.")
#define ZDB_HELP_DOC_LIST                                                                          \
	SHELL_HELP("List documents across every collection.",                                      \
		   "[limit] [from_ms to_ms]\n"                                                     \
		   "The window filters on updated_ms, which counts from boot.")
#define ZDB_HELP_DOC_SET SHELL_HELP("Set a typed field and save.", NULL)
#define ZDB_HELP_DOC_SET_I64                                                                       \
	SHELL_HELP("Set a 64-bit integer field.", "<collection> <document_id> <field> <value>")
#define ZDB_HELP_DOC_SET_F64                                                                       \
	SHELL_HELP("Set a double field.", "<collection> <document_id> <field> <value>")
#define ZDB_HELP_DOC_SET_STR                                                                       \
	SHELL_HELP("Set a string field.", "<collection> <document_id> <field> <text>")
#define ZDB_HELP_DOC_SET_BOOL                                                                      \
	SHELL_HELP("Set a boolean field.",                                                         \
		   "<collection> <document_id> <field> true|false|1|0|yes|no|on|off")
#define ZDB_HELP_DOC_SET_BYTES                                                                     \
	SHELL_HELP("Set a raw-bytes field from hex digits.",                                       \
		   "<collection> <document_id> <field> <hexdigits>")
#define ZDB_HELP_DOC_FIND SHELL_HELP("Find documents by an equal field value.", NULL)
#define ZDB_HELP_DOC_FIND_I64                                                                      \
	SHELL_HELP("Find documents whose integer field equals a value.", "<field> <value> [limit]")
#define ZDB_HELP_DOC_FIND_F64                                                                      \
	SHELL_HELP("Find documents whose double field equals a value.", "<field> <value> [limit]")
#define ZDB_HELP_DOC_FIND_STR                                                                      \
	SHELL_HELP("Find documents whose string field equals a value.", "<field> <text> [limit]")
#define ZDB_HELP_DOC_FIND_BOOL                                                                     \
	SHELL_HELP("Find documents whose boolean field equals a value.", "<field> <value> [limit]")

/*
 * <stdbool.h> defines bool as a macro, and SHELL_SUBCMD_ADD both stringifies
 * its syntax argument and pastes it into a symbol name — so a bare "bool"
 * would register the command as "_Bool". Nothing below needs the type.
 */
#undef bool

SHELL_SUBCMD_SET_CREATE(zdb_doc_set_cmds, (zdb, doc, set));
SHELL_SUBCMD_ADD((zdb, doc, set), i64, NULL, ZDB_HELP_DOC_SET_I64, cmd_zdb_doc_set_i64, 5, 0);
SHELL_SUBCMD_ADD((zdb, doc, set), f64, NULL, ZDB_HELP_DOC_SET_F64, cmd_zdb_doc_set_f64, 5, 0);
SHELL_SUBCMD_ADD((zdb, doc, set), str, NULL, ZDB_HELP_DOC_SET_STR, cmd_zdb_doc_set_str, 5, 0);
SHELL_SUBCMD_ADD((zdb, doc, set), bool, NULL, ZDB_HELP_DOC_SET_BOOL, cmd_zdb_doc_set_boolean, 5, 0);
SHELL_SUBCMD_ADD((zdb, doc, set), bytes, NULL, ZDB_HELP_DOC_SET_BYTES, cmd_zdb_doc_set_bytes, 5, 0);

SHELL_SUBCMD_SET_CREATE(zdb_doc_find_cmds, (zdb, doc, find));
SHELL_SUBCMD_ADD((zdb, doc, find), i64, NULL, ZDB_HELP_DOC_FIND_I64, cmd_zdb_doc_find_i64, 3, 1);
SHELL_SUBCMD_ADD((zdb, doc, find), f64, NULL, ZDB_HELP_DOC_FIND_F64, cmd_zdb_doc_find_f64, 3, 1);
SHELL_SUBCMD_ADD((zdb, doc, find), str, NULL, ZDB_HELP_DOC_FIND_STR, cmd_zdb_doc_find_str, 3, 1);
SHELL_SUBCMD_ADD((zdb, doc, find), bool, NULL, ZDB_HELP_DOC_FIND_BOOL, cmd_zdb_doc_find_boolean, 3, 1);

SHELL_SUBCMD_SET_CREATE(zdb_doc_cmds, (zdb, doc));
SHELL_SUBCMD_ADD((zdb, doc), create, NULL, ZDB_HELP_DOC_CREATE, cmd_zdb_doc_create, 3, 0);
SHELL_SUBCMD_ADD((zdb, doc), delete, NULL, ZDB_HELP_DOC_DELETE, cmd_zdb_doc_delete, 3, 0);
SHELL_SUBCMD_ADD((zdb, doc), get, NULL, ZDB_HELP_DOC_GET, cmd_zdb_doc_get, 3, 1);
SHELL_SUBCMD_ADD((zdb, doc), list, NULL, ZDB_HELP_DOC_LIST, cmd_zdb_doc_list, 1, 3);
SHELL_SUBCMD_ADD((zdb, doc), set, &zdb_doc_set_cmds, ZDB_HELP_DOC_SET, NULL, 1, 0);
SHELL_SUBCMD_ADD((zdb, doc), find, &zdb_doc_find_cmds, ZDB_HELP_DOC_FIND, NULL, 1, 0);

ZDB_SHELL_CMD_ADD(doc, &zdb_doc_cmds, ZDB_HELP_DOC, NULL, 1, 0);
