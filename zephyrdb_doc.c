/* DOC module implementation */
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)

#include "zephyrdb_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/kernel.h>

struct zdb_doc_hdr_v1 {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t field_count_le;
	uint64_t created_ms_le;
	uint64_t updated_ms_le;
	uint32_t crc_le;
} __packed;

struct zdb_doc_field_hdr_v1 {
	uint16_t name_len_le;
	uint16_t reserved_le;
	uint8_t type;
	uint8_t reserved[3];
} __packed;

#define ZDB_DOC_PATH_MAX 160
#define ZDB_DOC_DIRNAME "zdb_docs"
#define ZDB_DOC_FILE_EXT ".zdoc"
#define ZDB_DOC_MAGIC 0x5A444F43u
#define ZDB_DOC_VERSION 1u

static int zdb_doc_build_root_path(const zdb_cfg_t *cfg, char *path, size_t path_len);
static int zdb_doc_build_collection_path(const zdb_cfg_t *cfg, const char *collection, char *path,
						 size_t path_len);
static int zdb_doc_build_doc_path(const zdb_cfg_t *cfg, const char *collection,
					  const char *document_id, char *path, size_t path_len);
static int zdb_doc_ensure_dirs(const zdb_cfg_t *cfg, const char *collection);

static void *zdb_alloc_copy(const void *src, size_t len)
{
	void *dst;

	if ((src == NULL) || (len == 0U)) {
		return NULL;
	}

	dst = k_malloc(len);
	if (dst == NULL) {
		return NULL;
	}

	(void)memcpy(dst, src, len);
	return dst;
}

static zdb_status_t zdb_fs_read_exact(struct fs_file_t *file, void *buf, size_t len)
{
	ssize_t rd;

	rd = fs_read(file, buf, len);
	if (rd < 0) {
		return zdb_status_from_errno((int)rd);
	}

	if (rd != (ssize_t)len) {
		return ZDB_ERR_CORRUPT;
	}

	return ZDB_OK;
}

static char *zdb_strdup_local(const char *s)
{
	size_t n;
	char *dst;

	if (s == NULL) {
		return NULL;
	}

	n = strlen(s) + 1U;
	dst = zdb_alloc_copy(s, n);
	return dst;
}

/**
 * @brief Free allocated metadata from a query result entry.
 *
 * @param metadata Metadata entry to free
 */
static inline void zdb_doc_metadata_entry_free(zdb_doc_metadata_t *metadata)
{
	if (metadata == NULL) {
		return;
	}
	if (metadata->collection_name != NULL) {
		k_free((void *)metadata->collection_name);
		metadata->collection_name = NULL;
	}
	if (metadata->document_id != NULL) {
		k_free((void *)metadata->document_id);
		metadata->document_id = NULL;
	}
}

/*
 * Deserialize document from filesystem storage.
 *
 * Reads and parses the binary document format (see zdb_doc_save for format spec).
 * Validates magic number and version; returns ZDB_ERR_CORRUPT if invalid.
 * Each field is deserialized based on its type flag and added to the output document.
 *
 * If take_read_lock is true, acquires database read lock for file access safety.
 * Caller should set take_read_lock=true for public API calls to ensure correct
 * visibility and atomic reads; internal queries may use false to avoid deadlock.
 *
 * Errors:
 *   - ZDB_ERR_NOT_FOUND: document file doesn't exist
 *   - ZDB_ERR_CORRUPT: invalid magic, version, name length, or field count
 *   - ZDB_ERR_NOMEM: insufficient memory for fields or field names
 *   - ZDB_ERR_IO: read errors
 *
 * On error, out_doc is partially initialized and must be freed; caller should call
 * zdb_doc_close(out_doc) to release resources safely.
 */
static zdb_status_t zdb_doc_open_impl(zdb_t *db, const char *collection_name,
				      const char *document_id, zdb_doc_t *out_doc,
				      bool take_read_lock)
{
	char path[ZDB_DOC_PATH_MAX];
	struct fs_file_t file;
	struct zdb_doc_hdr_v1 hdr;
	struct zdb_doc_field_hdr_v1 field_hdr;
	zdb_status_t lock_rc = ZDB_OK;
	uint64_t saved_created_ms, saved_updated_ms;
	int rc = 0;
	size_t i;
	bool lock_held = false;
	bool file_open = false;
	zdb_status_t ret = ZDB_OK;

	rc = zdb_doc_create(db, collection_name, document_id, out_doc);
	if (rc != ZDB_OK) {
		return rc;
	}

	if (take_read_lock && (db != NULL)) {
		lock_rc = zdb_lock_read(db);
		if (lock_rc != ZDB_OK) {
			(void)zdb_doc_close(out_doc);
			return lock_rc;
		}
		lock_held = true;
	}

	rc = zdb_doc_build_doc_path(db->cfg, collection_name, document_id, path, sizeof(path));
	if (rc < 0) {
		ret = zdb_status_from_errno(rc);
		goto cleanup;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc < 0) {
		ret = zdb_status_from_errno(rc);
		goto cleanup;
	}
	file_open = true;

	/* Read and validate document header (magic, version, field count, timestamps) */
	ret = zdb_fs_read_exact(&file, &hdr, sizeof(hdr));
	if (ret != ZDB_OK) {
		goto cleanup;
	}

	if ((sys_le32_to_cpu(hdr.magic_le) != ZDB_DOC_MAGIC) ||
	    (sys_le16_to_cpu(hdr.version_le) != ZDB_DOC_VERSION)) {
		ret = ZDB_ERR_CORRUPT;
		goto cleanup;
	}

	{
		uint32_t expect_crc = crc32_ieee((const uint8_t *)&hdr,
						 offsetof(struct zdb_doc_hdr_v1, crc_le));
		uint32_t got_crc = sys_le32_to_cpu(hdr.crc_le);

		if (got_crc != expect_crc) {
			ret = ZDB_ERR_CORRUPT;
			goto cleanup;
		}
	}

	saved_created_ms = sys_le64_to_cpu(hdr.created_ms_le);
	saved_updated_ms = sys_le64_to_cpu(hdr.updated_ms_le);
	if ((size_t)sys_le16_to_cpu(hdr.field_count_le) > out_doc->max_fields) {
		ret = ZDB_ERR_NOMEM;
		goto cleanup;
	}

	/* Deserialize each field: read header, name, then type-specific value */
	for (i = 0U; i < (size_t)sys_le16_to_cpu(hdr.field_count_le); i++) {
		char *name = NULL;
		uint16_t name_len;
		zdb_doc_field_type_t type;

		ret = zdb_fs_read_exact(&file, &field_hdr, sizeof(field_hdr));
		if (ret != ZDB_OK) {
			goto cleanup;
		}

		name_len = sys_le16_to_cpu(field_hdr.name_len_le);
		type = (zdb_doc_field_type_t)field_hdr.type;
		if ((name_len == 0U) || (name_len > CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN)) {
			ret = ZDB_ERR_CORRUPT;
			goto cleanup;
		}

		name = k_malloc(name_len + 1U);
		if (name == NULL) {
			ret = ZDB_ERR_NOMEM;
			goto cleanup;
		}

		ret = zdb_fs_read_exact(&file, name, name_len);
		if (ret != ZDB_OK) {
			k_free(name);
			goto cleanup;
		}
		name[name_len] = '\0';

		/* Read and convert field value based on type from file format */
		switch (type) {
		case ZDB_DOC_FIELD_INT64: {
			uint64_t tmp_le;
			int64_t tmp;
			ret = zdb_fs_read_exact(&file, &tmp_le, sizeof(tmp_le));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			tmp = (int64_t)sys_le64_to_cpu(tmp_le);
			rc = zdb_doc_field_set_i64(out_doc, name, tmp);
			break;
		}
		case ZDB_DOC_FIELD_DOUBLE: {
			uint64_t tmp_le;
			double tmp;
			ret = zdb_fs_read_exact(&file, &tmp_le, sizeof(tmp_le));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			tmp_le = sys_le64_to_cpu(tmp_le);
			(void)memcpy(&tmp, &tmp_le, sizeof(tmp));
			rc = zdb_doc_field_set_f64(out_doc, name, tmp);
			break;
		}
		case ZDB_DOC_FIELD_BOOL: {
			uint8_t tmp;
			ret = zdb_fs_read_exact(&file, &tmp, sizeof(tmp));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			rc = zdb_doc_field_set_bool(out_doc, name, (tmp != 0U));
			break;
		}
		case ZDB_DOC_FIELD_STRING: {
			uint32_t str_len_le;
			size_t str_len;
			char *str;
			ret = zdb_fs_read_exact(&file, &str_len_le, sizeof(str_len_le));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			str_len = sys_le32_to_cpu(str_len_le);
			if (str_len > CONFIG_ZDB_DOC_MAX_STRING_LEN) {
				k_free(name);
				ret = ZDB_ERR_CORRUPT;
				goto cleanup;
			}
			str = k_malloc(str_len + 1U);
			if (str == NULL) {
				k_free(name);
				ret = ZDB_ERR_NOMEM;
				goto cleanup;
			}
			ret = zdb_fs_read_exact(&file, str, str_len);
			if (ret != ZDB_OK) {
				k_free(str);
				k_free(name);
				goto cleanup;
			}
			str[str_len] = '\0';
			rc = zdb_doc_field_set_string(out_doc, name, str);
			k_free(str);
			break;
		}
		case ZDB_DOC_FIELD_BYTES: {
			uint32_t bytes_len_le;
			size_t bytes_len;
			void *buf;
			ret = zdb_fs_read_exact(&file, &bytes_len_le, sizeof(bytes_len_le));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			bytes_len = sys_le32_to_cpu(bytes_len_le);
			if (bytes_len > CONFIG_ZDB_DOC_MAX_BYTES_LEN) {
				k_free(name);
				ret = ZDB_ERR_CORRUPT;
				goto cleanup;
			}
			buf = NULL;
			if (bytes_len > 0U) {
				buf = k_malloc(bytes_len);
				if (buf == NULL) {
					k_free(name);
					ret = ZDB_ERR_NOMEM;
					goto cleanup;
				}
				ret = zdb_fs_read_exact(&file, buf, bytes_len);
				if (ret != ZDB_OK) {
					k_free(buf);
					k_free(name);
					goto cleanup;
				}
			}
			rc = zdb_doc_field_set_bytes(out_doc, name, buf, bytes_len);
			k_free(buf);
			break;
		}
		default:
			rc = ZDB_ERR_UNSUPPORTED;
			break;
		}

		k_free(name);
		if (rc != ZDB_OK) {
			ret = rc;
			goto cleanup;
		}
	}

cleanup:
	if (file_open) {
		(void)fs_close(&file);
	}

	if ((ret == ZDB_OK) && (out_doc != NULL)) {
		out_doc->created_ms = saved_created_ms;
		out_doc->updated_ms = saved_updated_ms;
	} else if (out_doc != NULL) {
		(void)zdb_doc_close(out_doc);
	}

	if (lock_held && (db != NULL)) {
		zdb_unlock_read(db);
	}

	return ret;
}

static void zdb_doc_field_payload_free(zdb_doc_field_t *field)
{
	if (field == NULL) {
		return;
	}

	if ((field->type == ZDB_DOC_FIELD_STRING) && (field->value.str != NULL)) {
		k_free((void *)field->value.str);
		field->value.str = NULL;
	}

	if ((field->type == ZDB_DOC_FIELD_BYTES) && (field->value.bytes.data != NULL)) {
		k_free((void *)field->value.bytes.data);
		field->value.bytes.data = NULL;
		field->value.bytes.len = 0U;
	}
}

static void zdb_doc_field_value_free(zdb_doc_field_t *field)
{
	if (field == NULL) {
		return;
	}

	if (field->name != NULL) {
		k_free((void *)field->name);
		field->name = NULL;
	}

	zdb_doc_field_payload_free(field);
}

static void zdb_doc_fields_reset(zdb_doc_t *doc)
{
	size_t i;

	if ((doc == NULL) || (doc->fields == NULL)) {
		return;
	}

	for (i = 0U; i < doc->field_count; i++) {
		zdb_doc_field_value_free(&doc->fields[i]);
	}

	(void)memset(doc->fields, 0, sizeof(zdb_doc_field_t) * doc->max_fields);
	doc->field_count = 0U;
}

static int zdb_doc_build_root_path(const zdb_cfg_t *cfg, char *path, size_t path_len)
{
	int n;

	if ((cfg == NULL) || (cfg->lfs_mount_point == NULL) || (path == NULL) || (path_len == 0U)) {
		return -EINVAL;
	}

	n = snprintf(path, path_len, "%s/%s", cfg->lfs_mount_point, ZDB_DOC_DIRNAME);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static int zdb_doc_build_collection_path(const zdb_cfg_t *cfg, const char *collection, char *path,
						 size_t path_len)
{
	int n;

	if ((collection == NULL) || (collection[0] == '\0')) {
		return -EINVAL;
	}

	n = snprintf(path, path_len, "%s/%s/%s", cfg->lfs_mount_point, ZDB_DOC_DIRNAME, collection);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static int zdb_doc_build_doc_path(const zdb_cfg_t *cfg, const char *collection, const char *document_id,
					  char *path, size_t path_len)
{
	int n;

	if ((cfg == NULL) || (collection == NULL) || (document_id == NULL) || (path == NULL) ||
	    (path_len == 0U)) {
		return -EINVAL;
	}

	n = snprintf(path, path_len, "%s/%s/%s/%s%s", cfg->lfs_mount_point, ZDB_DOC_DIRNAME, collection,
		     document_id, ZDB_DOC_FILE_EXT);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static int zdb_doc_ensure_dirs(const zdb_cfg_t *cfg, const char *collection)
{
	char root_path[ZDB_DOC_PATH_MAX];
	char collection_path[ZDB_DOC_PATH_MAX];
	int rc;

	rc = zdb_doc_build_root_path(cfg, root_path, sizeof(root_path));
	if (rc < 0) {
		return rc;
	}

	rc = fs_mkdir(root_path);
	if ((rc < 0) && (rc != -EEXIST)) {
		return rc;
	}

	rc = zdb_doc_build_collection_path(cfg, collection, collection_path, sizeof(collection_path));
	if (rc < 0) {
		return rc;
	}

	rc = fs_mkdir(collection_path);
	if ((rc < 0) && (rc != -EEXIST)) {
		return rc;
	}

	return 0;
}

static bool zdb_doc_field_filter_match(const zdb_doc_field_t *field,
					       const zdb_doc_query_filter_t *filter)
{
	double ipart;

	if ((field == NULL) || (filter == NULL)) {
		return false;
	}

	if ((field->name == NULL) || (field->name[0] == '\0') ||
	    (filter->field_name == NULL) || (filter->field_name[0] == '\0')) {
		return false;
	}

	if (strcmp(field->name, filter->field_name) != 0) {
		return false;
	}

	if (field->type != filter->type) {
		return false;
	}

	switch (field->type) {
	case ZDB_DOC_FIELD_INT64:
		if ((filter->numeric_value < (double)INT64_MIN) ||
		    (filter->numeric_value > (double)INT64_MAX)) {
			return false;
		}
		return (modf(filter->numeric_value, &ipart) == 0.0) &&
		       (field->value.i64 == (int64_t)ipart);
	case ZDB_DOC_FIELD_DOUBLE:
		return field->value.f64 == filter->numeric_value;
	case ZDB_DOC_FIELD_BOOL:
		return (field->value.b == filter->bool_value);
	case ZDB_DOC_FIELD_STRING:
		return (field->value.str != NULL) && (filter->string_value != NULL) &&
		       (strcmp(field->value.str, filter->string_value) == 0);
	case ZDB_DOC_FIELD_BYTES:
		if ((filter->string_value == NULL) || (field->value.bytes.data == NULL)) {
			return false;
		}
		return (field->value.bytes.len == strlen(filter->string_value)) &&
		       (memcmp(field->value.bytes.data, filter->string_value,
			       field->value.bytes.len) == 0);
	default:
		return false;
	}
}

static bool zdb_doc_matches_query(const zdb_doc_t *doc, const zdb_doc_query_t *query)
{
	size_t i;
	size_t j;
	bool matched;

	if ((doc == NULL) || (query == NULL)) {
		return false;
	}

	if ((query->from_ms != 0U) && (doc->updated_ms < query->from_ms)) {
		return false;
	}

	if ((query->to_ms != 0U) && (doc->updated_ms > query->to_ms)) {
		return false;
	}

	if ((query->filter_count > 0U) && (query->filters == NULL)) {
		return false;
	}

	for (i = 0U; i < query->filter_count; i++) {
		matched = false;
		for (j = 0U; j < doc->field_count; j++) {
			if (zdb_doc_field_filter_match(&doc->fields[j], &query->filters[i])) {
				matched = true;
				break;
			}
		}
		if (!matched) {
			return false;
		}
	}

	return true;
}

/**
 * @brief Validate a name component for use in filesystem paths.
 *
 * Rejects names containing path separators, parent directory references,
 * or embedded null bytes that could enable directory traversal.
 */
static bool zdb_doc_name_valid(const char *name)
{
	const char *p;

	if ((name == NULL) || (name[0] == '\0')) {
		return false;
	}

	if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
		return false;
	}

	for (p = name; *p != '\0'; p++) {
		if ((*p == '/') || (*p == '\\')) {
			return false;
		}
	}

	return true;
}

zdb_status_t zdb_doc_create(zdb_t *db, const char *collection_name,
			     const char *document_id, zdb_doc_t *out_doc)
{
	char *collection_dup;
	char *document_dup;
	if ((db == NULL) || (collection_name == NULL) || (document_id == NULL) ||
	    (out_doc == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->cfg == NULL) || (db->cfg->lfs_mount_point == NULL)) {
		return ZDB_ERR_INVAL;
	}

	/* Validate collection_name and document_id against CONFIG_ZDB_MAX_KEY_LEN */
	if ((strlen(collection_name) == 0U) || (strlen(collection_name) > CONFIG_ZDB_MAX_KEY_LEN) ||
	    (strlen(document_id) == 0U) || (strlen(document_id) > CONFIG_ZDB_MAX_KEY_LEN)) {
		return ZDB_ERR_INVAL;
	}

	/* Reject path traversal in collection/document names */
	if (!zdb_doc_name_valid(collection_name) || !zdb_doc_name_valid(document_id)) {
		return ZDB_ERR_INVAL;
	}

	collection_dup = zdb_strdup_local(collection_name);
	document_dup = zdb_strdup_local(document_id);
	if ((collection_dup == NULL) || (document_dup == NULL)) {
		k_free(collection_dup);
		k_free(document_dup);
		return ZDB_ERR_NOMEM;
	}

	/* Initialize document */
	out_doc->db = db;
	out_doc->collection_name = collection_dup;
	out_doc->document_id = document_dup;
	out_doc->fields = NULL;
	out_doc->field_count = 0;
	out_doc->max_fields = 0;
	out_doc->created_ms = k_uptime_get();
	out_doc->updated_ms = out_doc->created_ms;
	out_doc->valid = false;

	/* Allocate field array (stage 3: use slab) */
	out_doc->max_fields = CONFIG_ZDB_DOC_MAX_FIELD_COUNT;
	out_doc->fields = k_calloc(out_doc->max_fields, sizeof(zdb_doc_field_t));
	if (out_doc->fields == NULL) {
		k_free((void *)out_doc->collection_name);
		k_free((void *)out_doc->document_id);
		out_doc->db = NULL;
		out_doc->collection_name = NULL;
		out_doc->document_id = NULL;
		out_doc->fields = NULL;
		out_doc->field_count = 0U;
		out_doc->max_fields = 0U;
		out_doc->created_ms = 0U;
		out_doc->updated_ms = 0U;
		out_doc->valid = false;
		return ZDB_ERR_NOMEM;
	}

	out_doc->valid = true;
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_doc_event(db, ZDB_DOC_EVENT_CREATE, collection_name, document_id,
			   out_doc->field_count, 0U, ZDB_OK);
#endif
	return ZDB_OK;
}

zdb_status_t zdb_doc_open(zdb_t *db, const char *collection_name,
			   const char *document_id, zdb_doc_t *out_doc)
{
	return zdb_doc_open_impl(db, collection_name, document_id, out_doc, true);
}

/*
 * Serialize document to persistent storage (filesystem).
 *
 * Binary Format (little-endian throughout):
 *   - Header (32 bytes): magic | version | field_count | created_ms | updated_ms | crc
 *   - For each field:
 *     - Field header (8 bytes): name_len | reserved | type | reserved[3]
 *     - Field name (name_len bytes): UTF-8 null-terminated string
 *     - Field value (type-dependent):
 *       - INT64 (8 bytes): signed 64-bit integer
 *       - DOUBLE (8 bytes): IEEE-754 double
 *       - BOOL (1 byte): 0 or 1
 *       - STRING: 4-byte length prefix + null-terminated UTF-8 string
 *       - BYTES: 4-byte length prefix + raw binary data
 *
 * Error Behavior:
 *   - Invalid inputs or state return ZDB_ERR_INVAL
 *   - Memory/filesystem issues return ZDB_ERR_NOMEM or ZDB_ERR_IO
 *   - Unsupported field types return ZDB_ERR_UNSUPPORTED
 *
 *   On partial write failure, file is closed and write lock released;
 *   caller must handle incomplete file on disk.
 */
zdb_status_t zdb_doc_save(zdb_doc_t *doc)
{
	char path[ZDB_DOC_PATH_MAX];
	struct fs_file_t file;
	struct zdb_doc_hdr_v1 hdr;
	struct zdb_doc_field_hdr_v1 fh;
	zdb_status_t lock_rc;
	int rc;
	ssize_t wr;
	size_t i;
	size_t serialized_bytes = 0U;

	if ((doc == NULL) || (!doc->valid)) {
		return ZDB_ERR_INVAL;
	}

	doc->updated_ms = k_uptime_get();
	if (doc->db == NULL || doc->db->cfg == NULL) {
		return ZDB_ERR_INVAL;
	}

	lock_rc = zdb_lock_write(doc->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rc = zdb_doc_ensure_dirs(doc->db->cfg, doc->collection_name);
	if (rc < 0) {
		zdb_unlock_write(doc->db);
		return zdb_status_from_errno(rc);
	}

	rc = zdb_doc_build_doc_path(doc->db->cfg, doc->collection_name, doc->document_id, path,
				    sizeof(path));
	if (rc < 0) {
		zdb_unlock_write(doc->db);
		return zdb_status_from_errno(rc);
	}

	/* Build document header with magic, version, counts, and timestamps */
	hdr.magic_le = sys_cpu_to_le32(ZDB_DOC_MAGIC);
	hdr.version_le = sys_cpu_to_le16(ZDB_DOC_VERSION);
	hdr.field_count_le = sys_cpu_to_le16((uint16_t)doc->field_count);
	hdr.created_ms_le = sys_cpu_to_le64(doc->created_ms);
	hdr.updated_ms_le = sys_cpu_to_le64(doc->updated_ms);
	hdr.crc_le = sys_cpu_to_le32(
		crc32_ieee((const uint8_t *)&hdr,
			   offsetof(struct zdb_doc_hdr_v1, crc_le)));

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc < 0) {
		zdb_unlock_write(doc->db);
		return zdb_status_from_errno(rc);
	}

	/* Write header to file */
	wr = fs_write(&file, &hdr, sizeof(hdr));
	if (wr != (ssize_t)sizeof(hdr)) {
		(void)fs_close(&file);
		zdb_unlock_write(doc->db);
		return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
	}

	/* Serialize each field: header, name, then type-specific value */
	for (i = 0U; i < doc->field_count; i++) {
		const zdb_doc_field_t *field = &doc->fields[i];
		uint16_t name_len = (uint16_t)strlen(field->name);

		/* Write field header with type and name length */
		fh.name_len_le = sys_cpu_to_le16(name_len);
		fh.reserved_le = 0U;
		fh.type = (uint8_t)field->type;
		fh.reserved[0] = fh.reserved[1] = fh.reserved[2] = 0U;

		wr = fs_write(&file, &fh, sizeof(fh));
		if (wr != (ssize_t)sizeof(fh)) {
			(void)fs_close(&file);
			zdb_unlock_write(doc->db);
			return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
		}

		/* Write field name */
		wr = fs_write(&file, field->name, name_len);
		if (wr != (ssize_t)name_len) {
			(void)fs_close(&file);
			zdb_unlock_write(doc->db);
			return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
		}

		/* Write field value based on type */
		switch (field->type) {
		case ZDB_DOC_FIELD_INT64: {
			uint64_t tmp_le = sys_cpu_to_le64((uint64_t)field->value.i64);
			wr = fs_write(&file, &tmp_le, sizeof(tmp_le));
			if (wr != (ssize_t)sizeof(tmp_le)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			break;
		}
		case ZDB_DOC_FIELD_DOUBLE: {
			/* Note: double is bit-copied as-is then endian-swapped to preserve IEEE-754 */
			uint64_t tmp_le;
			(void)memcpy(&tmp_le, &field->value.f64, sizeof(tmp_le));
			tmp_le = sys_cpu_to_le64(tmp_le);
			wr = fs_write(&file, &tmp_le, sizeof(tmp_le));
			if (wr != (ssize_t)sizeof(tmp_le)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			break;
		}
		case ZDB_DOC_FIELD_BOOL: {
			/* Store boolean as single byte: 0 or 1 */
			uint8_t b = field->value.b ? 1U : 0U;
			wr = fs_write(&file, &b, sizeof(b));
			if (wr != (ssize_t)sizeof(b)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			break;
		}
		case ZDB_DOC_FIELD_STRING: {
			/* Write variable-length string: 4-byte length prefix + data */
			uint32_t str_len = (uint32_t)strlen(field->value.str != NULL ? field->value.str : "");
			uint32_t str_len_le = sys_cpu_to_le32(str_len);
			wr = fs_write(&file, &str_len_le, sizeof(str_len_le));
			if (wr != (ssize_t)sizeof(str_len_le)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			if (str_len > 0U) {
				wr = fs_write(&file, field->value.str, str_len);
				if (wr != (ssize_t)str_len) {
					(void)fs_close(&file);
					zdb_unlock_write(doc->db);
					return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
				}
			}
			break;
		}
		case ZDB_DOC_FIELD_BYTES: {
			/* Write variable-length binary data: 4-byte length prefix + data */
			uint32_t bytes_len = (uint32_t)field->value.bytes.len;
			uint32_t bytes_len_le = sys_cpu_to_le32(bytes_len);
			wr = fs_write(&file, &bytes_len_le, sizeof(bytes_len_le));
			if (wr != (ssize_t)sizeof(bytes_len_le)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			if (bytes_len > 0U) {
				wr = fs_write(&file, field->value.bytes.data, bytes_len);
				if (wr != (ssize_t)bytes_len) {
					(void)fs_close(&file);
					zdb_unlock_write(doc->db);
					return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
				}
			}
			break;
		}
		default:
			(void)fs_close(&file);
			zdb_unlock_write(doc->db);
			return ZDB_ERR_UNSUPPORTED;
		}
	}

	/* Record final file size for event reporting */
	{
		off_t end_pos = fs_tell(&file);
		if (end_pos > 0) {
			serialized_bytes = (size_t)end_pos;
		}
	}

	(void)fs_close(&file);
	zdb_unlock_write(doc->db);
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	/* Emit save event with serialized size for monitoring */
	zdb_emit_doc_event(doc->db, ZDB_DOC_EVENT_SAVE, doc->collection_name, doc->document_id,
			   doc->field_count, serialized_bytes, ZDB_OK);
#endif
	return ZDB_OK;

}

/*
 * Delete document file from persistent storage.
 *
 * Acquires database write lock to prevent concurrent open/query operations.
 * Removes the document file from the filesystem. If the file doesn't exist,
 * returns success (idempotent deletion).
 *
 * Error behavior:
 *   - ZDB_ERR_INVAL: invalid pointers or collection/document names
 *   - ZDB_ERR_IO: filesystem errors during path building or unlink
 *
 * After successful deletion, the document is removed from storage.
 * Callers with open document handles should close them;
 * subsequent open() calls will fail with ZDB_ERR_NOT_FOUND.
 */
zdb_status_t zdb_doc_delete(zdb_t *db, const char *collection_name,
			     const char *document_id)
{
	char path[ZDB_DOC_PATH_MAX];
	zdb_status_t lock_rc;
	int rc;

	if ((db == NULL) || (collection_name == NULL) || (document_id == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->cfg == NULL) || (db->cfg->lfs_mount_point == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((strlen(collection_name) == 0U) || (strlen(collection_name) > CONFIG_ZDB_MAX_KEY_LEN) ||
	    (strlen(document_id) == 0U) || (strlen(document_id) > CONFIG_ZDB_MAX_KEY_LEN)) {
		return ZDB_ERR_INVAL;
	}

	if (!zdb_doc_name_valid(collection_name) || !zdb_doc_name_valid(document_id)) {
		return ZDB_ERR_INVAL;
	}

	/* Acquire write lock to prevent races with open/query */
	lock_rc = zdb_lock_write(db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rc = zdb_doc_build_doc_path(db->cfg, collection_name, document_id, path, sizeof(path));
	if (rc < 0) {
		zdb_unlock_write(db);
		return zdb_status_from_errno(rc);
	}

	rc = fs_unlink(path);
	zdb_unlock_write(db);
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_doc_event(db, ZDB_DOC_EVENT_DELETE, collection_name, document_id, 0U, 0U, ZDB_OK);
#endif
	return ZDB_OK;
}

zdb_status_t zdb_doc_close(zdb_doc_t *doc)
{
	if (doc == NULL) {
		return ZDB_ERR_INVAL;
	}

	zdb_doc_fields_reset(doc);

	if (doc->fields != NULL) {
		k_free(doc->fields);
		doc->fields = NULL;
	}

	if (doc->collection_name != NULL) {
		k_free((void *)doc->collection_name);
		doc->collection_name = NULL;
	}

	if (doc->document_id != NULL) {
		k_free((void *)doc->document_id);
		doc->document_id = NULL;
	}

	doc->valid = false;
	return ZDB_OK;
}

/*
 * Field setters
 */
static zdb_status_t zdb_doc_field_find_or_create(zdb_doc_t *doc,
						  const char *field_name,
						  zdb_doc_field_type_t type,
						  zdb_doc_field_t **out_field)
{
	size_t i;
	char *name_dup;
	zdb_doc_field_t *field = NULL;

	if ((doc == NULL) || (field_name == NULL) || (out_field == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!doc->valid) {
		return ZDB_ERR_INVAL;
	}

	/* Validate field_name against CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN */
	if ((strlen(field_name) == 0U) || (strlen(field_name) > CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN)) {
		return ZDB_ERR_INVAL;
	}

	for (i = 0U; i < doc->field_count; i++) {
		if ((doc->fields[i].name != NULL) &&
		    (strcmp(doc->fields[i].name, field_name) == 0)) {
			field = &doc->fields[i];
			break;
		}
	}

	if (field == NULL) {
		if (doc->field_count >= doc->max_fields) {
			return ZDB_ERR_NOMEM;
		}

		name_dup = zdb_strdup_local(field_name);
		if (name_dup == NULL) {
			return ZDB_ERR_NOMEM;
		}

		field = &doc->fields[doc->field_count++];
		(void)memset(field, 0, sizeof(*field));
		field->name = name_dup;
	} else if (field->type != type) {
		zdb_doc_field_payload_free(field);
		(void)memset(&field->value, 0, sizeof(field->value));
	}

	field->type = type;
	*out_field = field;
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_i64(zdb_doc_t *doc, const char *field_name,
				    int64_t value)
{
	zdb_doc_field_t *field = NULL;
	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_INT64, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	field->value.i64 = value;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_f64(zdb_doc_t *doc, const char *field_name,
				    double value)
{
	zdb_doc_field_t *field = NULL;
	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_DOUBLE, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	field->value.f64 = value;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_string(zdb_doc_t *doc, const char *field_name,
				       const char *value)
{
	zdb_doc_field_t *field = NULL;
	char *dup;

	if ((doc == NULL) || (field_name == NULL) || (value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (strlen(value) > (size_t)CONFIG_ZDB_DOC_MAX_STRING_LEN) {
		return ZDB_ERR_INVAL;
	}

	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_STRING, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	dup = zdb_strdup_local(value);
	if (dup == NULL) {
		return ZDB_ERR_NOMEM;
	}

	if ((field->value.str != NULL) && (field->type == ZDB_DOC_FIELD_STRING)) {
		k_free((void *)field->value.str);
	}
	field->value.str = dup;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_bool(zdb_doc_t *doc, const char *field_name,
				     bool value)
{
	zdb_doc_field_t *field = NULL;
	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_BOOL, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	field->value.b = value;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_bytes(zdb_doc_t *doc, const char *field_name,
				      const void *value, size_t len)
{
	zdb_doc_field_t *field = NULL;
	void *dup;

	if ((doc == NULL) || (field_name == NULL) || ((value == NULL) && (len > 0U))) {
		return ZDB_ERR_INVAL;
	}

	if (len > CONFIG_ZDB_DOC_MAX_BYTES_LEN) {
		return ZDB_ERR_INVAL;
	}

	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_BYTES, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	dup = zdb_alloc_copy(value, len);
	if ((len > 0U) && (dup == NULL)) {
		return ZDB_ERR_NOMEM;
	}

	if ((field->value.bytes.data != NULL) && (field->type == ZDB_DOC_FIELD_BYTES)) {
		k_free((void *)field->value.bytes.data);
	}
	field->value.bytes.data = dup;
	field->value.bytes.len = len;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

/*
 * Field getters
 */
zdb_status_t zdb_doc_field_get_i64(const zdb_doc_t *doc, const char *field_name,
				    int64_t *out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_INT64) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.i64;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_doc_field_get_f64(const zdb_doc_t *doc, const char *field_name,
				    double *out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_DOUBLE) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.f64;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_doc_field_get_string(const zdb_doc_t *doc, const char *field_name,
				       const char **out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_STRING) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.str;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_doc_field_get_bool(const zdb_doc_t *doc, const char *field_name,
				     bool *out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_BOOL) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.b;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_doc_field_get_bytes(const zdb_doc_t *doc, const char *field_name,
				      zdb_bytes_t *out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_BYTES) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.bytes;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

/*
 * Query and export
 */
zdb_status_t zdb_doc_query(zdb_t *db, const zdb_doc_query_t *query,
			    zdb_doc_metadata_t *out_metadata, size_t *out_count)
{
	char root_path[ZDB_DOC_PATH_MAX];
	struct fs_dir_t dir_collection;
	struct fs_dir_t dir_docs;
	struct fs_dirent collection_ent;
	struct fs_dirent doc_ent;
	zdb_status_t lock_rc;
	int rc;
	size_t capacity;
	size_t matched = 0U;
	size_t written = 0U;
	bool stop_scan = false;
	uint32_t limit;

	if ((db == NULL) || (query == NULL) || (out_count == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->cfg == NULL) || (db->cfg->lfs_mount_point == NULL)) {
		return ZDB_ERR_INVAL;
	}

	capacity = *out_count;
	limit = (query->limit == 0U) ? UINT32_MAX : query->limit;

	lock_rc = zdb_lock_read(db);
	if (lock_rc != ZDB_OK) {
		*out_count = 0U;
		return lock_rc;
	}

	rc = zdb_doc_build_root_path(db->cfg, root_path, sizeof(root_path));
	if (rc < 0) {
		zdb_unlock_read(db);
		return zdb_status_from_errno(rc);
	}

	fs_dir_t_init(&dir_collection);
	rc = fs_opendir(&dir_collection, root_path);
	if (rc < 0) {
		*out_count = 0U;
		zdb_unlock_read(db);
		if (rc == -ENOENT) {
			return ZDB_OK;
		}
		return zdb_status_from_errno(rc);
	}

	while (!stop_scan && (matched < limit) &&
	       (fs_readdir(&dir_collection, &collection_ent) == 0) &&
	       (collection_ent.name[0] != '\0')) {
		char collection_path[ZDB_DOC_PATH_MAX];

		if (collection_ent.type != FS_DIR_ENTRY_DIR) {
			continue;
		}

		rc = zdb_doc_build_collection_path(db->cfg, collection_ent.name, collection_path,
						   sizeof(collection_path));
		if (rc < 0) {
			continue;
		}

		fs_dir_t_init(&dir_docs);
		rc = fs_opendir(&dir_docs, collection_path);
		if (rc < 0) {
			continue;
		}

		while (!stop_scan && (matched < limit) &&
		       (fs_readdir(&dir_docs, &doc_ent) == 0) &&
		       (doc_ent.name[0] != '\0')) {
			size_t name_len;
			char doc_id[CONFIG_ZDB_MAX_KEY_LEN + 1];
			zdb_doc_t doc;

			if (doc_ent.type != FS_DIR_ENTRY_FILE) {
				continue;
			}

			name_len = strlen(doc_ent.name);
			if ((name_len <= strlen(ZDB_DOC_FILE_EXT)) ||
			    (strcmp(doc_ent.name + name_len - strlen(ZDB_DOC_FILE_EXT), ZDB_DOC_FILE_EXT) != 0)) {
				continue;
			}

			if (name_len - strlen(ZDB_DOC_FILE_EXT) > CONFIG_ZDB_MAX_KEY_LEN) {
				continue;
			}

			(void)memcpy(doc_id, doc_ent.name, name_len - strlen(ZDB_DOC_FILE_EXT));
			doc_id[name_len - strlen(ZDB_DOC_FILE_EXT)] = '\0';

			rc = zdb_doc_open_impl(db, collection_ent.name, doc_id, &doc, false);
			if (rc != ZDB_OK) {
				continue;
			}

			if (zdb_doc_matches_query(&doc, query)) {
				if ((out_metadata != NULL) && (written < capacity)) {
					out_metadata[written].collection_name = zdb_strdup_local(collection_ent.name);
					if (out_metadata[written].collection_name == NULL) {
					/* Allocation failed; return partial results */
					(void)zdb_doc_close(&doc);
					(void)fs_closedir(&dir_docs);
					(void)fs_closedir(&dir_collection);
					zdb_unlock_read(db);
					*out_count = written;
					return ZDB_OK;
					}
					out_metadata[written].document_id = zdb_strdup_local(doc_id);
					if (out_metadata[written].document_id == NULL) {
					/* Allocation failed; free collection_name and return partial results */
					zdb_doc_metadata_entry_free(&out_metadata[written]);
					(void)zdb_doc_close(&doc);
					(void)fs_closedir(&dir_docs);
					(void)fs_closedir(&dir_collection);
					zdb_unlock_read(db);
					*out_count = written;
					return ZDB_OK;
					}

					out_metadata[written].created_ms = doc.created_ms;
					out_metadata[written].updated_ms = doc.updated_ms;
					out_metadata[written].field_count = (uint32_t)doc.field_count;
					written++;
					if (written >= capacity) {
						stop_scan = true;
					}
				}
				matched++;
			}

			(void)zdb_doc_close(&doc);
		}

		(void)fs_closedir(&dir_docs);
	}

	(void)fs_closedir(&dir_collection);
	zdb_unlock_read(db);
	*out_count = ((out_metadata == NULL) || (capacity == 0U)) ? matched : written;
	return ZDB_OK;
}

zdb_status_t zdb_doc_metadata_free(zdb_doc_metadata_t *metadata, size_t count)
{
	size_t i;

	if ((metadata == NULL) && (count > 0U)) {
		return ZDB_ERR_INVAL;
	}

	for (i = 0U; i < count; i++) {
		zdb_doc_metadata_entry_free(&metadata[i]);
	}

	return ZDB_OK;
}

zdb_status_t zdb_doc_export_flatbuffer(zdb_doc_t *doc, uint8_t *out_buf,
				       size_t out_capacity, size_t *out_len)
{
	if ((doc == NULL) || (out_len == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!doc->valid) {
		return ZDB_ERR_INVAL;
	}

	/* Stage 3: FlatBuffer export not yet implemented */
	ARG_UNUSED(out_buf);
	ARG_UNUSED(out_capacity);
	*out_len = 0U;
	return ZDB_ERR_UNSUPPORTED;
}

#endif /* CONFIG_ZDB_DOC */
