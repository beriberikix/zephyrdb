/* KV module implementation */
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)

#include "zephyrdb_internal.h"

#include <errno.h>
#include <string.h>

#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
#include <zephyr/kvss/nvs.h>
#endif
#if defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
#include <zephyr/kvss/zms.h>
#endif

#define ZDB_KV_INDEX_MAX_ENTRIES 128U

struct zdb_kv_index_entry {
	char namespace_name[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	uint32_t id;
};

struct zdb_kv_ctx {
	struct zdb_kv_index_entry entries[ZDB_KV_INDEX_MAX_ENTRIES];
	size_t entry_count;
};

static bool zdb_key_valid(const char *key)
{
	size_t key_len;

	if ((key == NULL) || ((*key) == '\0')) {
		return false;
	}

	key_len = strlen(key);
	return (key_len <= (size_t)CONFIG_ZDB_MAX_KEY_LEN);
}

/*
 * On-disk KV record format (v2):
 *   [tag: 0xDB] [ns_len: 1 byte] [key_len: 1 byte]
 *   [namespace: ns_len bytes] [key: key_len bytes] [value: remaining bytes]
 *
 * ns_len/key_len are string lengths excluding NUL terminators; both are
 * bounded by CONFIG_ZDB_MAX_KEY_LEN (<= 128), so the tag byte 0xDB (219)
 * can never be a valid first byte of a v1 record ([key_len][key][value]).
 * Records whose first byte is not the tag are treated as absent: reads
 * and deletes report NOT_FOUND, and a set reclaims the slot.
 *
 * The backend record ID is FNV-1a over "namespace \0 key" (the NUL
 * separator keeps the mapping injective), folded to 16 bits for NVS.
 * A set whose slot already holds a different (namespace, key) fails
 * with ZDB_ERR_COLLISION and leaves the stored record untouched.
 */
#define ZDB_KV_REC_TAG      0xDBu
#define ZDB_KV_REC_HDR_SIZE 3U

enum zdb_kv_rec_check {
	ZDB_KV_REC_MATCH,    /* valid v2 record for this (namespace, key) */
	ZDB_KV_REC_MISMATCH, /* valid v2 record for a different (namespace, key) */
	ZDB_KV_REC_FOREIGN,  /* not a v2 record (old format or garbage) */
};

static uint32_t zdb_fnv1a32_update(uint32_t state, const uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0U; i < len; i++) {
		state ^= buf[i];
		state *= 0x01000193u;
	}

	return state;
}

static uint32_t zdb_kv_record_id(const char *namespace_name, const char *key)
{
	static const uint8_t separator = 0x00U;
	uint32_t hash = 0x811C9DC5u;

	hash = zdb_fnv1a32_update(hash, (const uint8_t *)namespace_name,
				  strlen(namespace_name));
	hash = zdb_fnv1a32_update(hash, &separator, sizeof(separator));
	hash = zdb_fnv1a32_update(hash, (const uint8_t *)key, strlen(key));

#if !defined(CONFIG_ZDB_KV_BACKEND_ZMS) || !(CONFIG_ZDB_KV_BACKEND_ZMS)
	/* NVS record IDs are 16-bit. */
	hash &= 0xFFFFu;
#endif

	/* Avoid ID 0, which can be reserved by backends. */
	if (hash == 0U) {
		hash = 1U;
	}

	return hash;
}

static size_t zdb_kv_record_build(uint8_t *buf, const char *namespace_name,
				  const char *key, const void *value, size_t value_len)
{
	size_t ns_len = strlen(namespace_name);
	size_t key_len = strlen(key);

	buf[0] = (uint8_t)ZDB_KV_REC_TAG;
	buf[1] = (uint8_t)ns_len;
	buf[2] = (uint8_t)key_len;
	(void)memcpy(&buf[ZDB_KV_REC_HDR_SIZE], namespace_name, ns_len);
	(void)memcpy(&buf[ZDB_KV_REC_HDR_SIZE + ns_len], key, key_len);
	/* A zero-length value may come with a NULL pointer; memcpy would be UB. */
	if (value_len > 0U) {
		(void)memcpy(&buf[ZDB_KV_REC_HDR_SIZE + ns_len + key_len], value, value_len);
	}

	return ZDB_KV_REC_HDR_SIZE + ns_len + key_len + value_len;
}

static enum zdb_kv_rec_check zdb_kv_record_check(const uint8_t *buf, size_t len,
						 const char *namespace_name,
						 const char *key, size_t *out_value_off)
{
	size_t ns_len;
	size_t key_len;

	if ((len < ZDB_KV_REC_HDR_SIZE) || (buf[0] != (uint8_t)ZDB_KV_REC_TAG)) {
		return ZDB_KV_REC_FOREIGN;
	}

	ns_len = (size_t)buf[1];
	key_len = (size_t)buf[2];
	if (len < (ZDB_KV_REC_HDR_SIZE + ns_len + key_len)) {
		return ZDB_KV_REC_FOREIGN;
	}

	if ((ns_len != strlen(namespace_name)) || (key_len != strlen(key)) ||
	    (memcmp(&buf[ZDB_KV_REC_HDR_SIZE], namespace_name, ns_len) != 0) ||
	    (memcmp(&buf[ZDB_KV_REC_HDR_SIZE + ns_len], key, key_len) != 0)) {
		return ZDB_KV_REC_MISMATCH;
	}

	if (out_value_off != NULL) {
		*out_value_off = ZDB_KV_REC_HDR_SIZE + ns_len + key_len;
	}

	return ZDB_KV_REC_MATCH;
}

static void *zdb_kv_backend_fs_from_db(zdb_t *db)
{
	if ((db == NULL) || (db->cfg == NULL)) {
		return NULL;
	}

	/*
	 * cfg->kv_backend_fs points at an initialized backend fs mounted by
	 * board/application startup:
	 * - struct nvs_fs when CONFIG_ZDB_KV_BACKEND_NVS=y
	 * - struct zms_fs when CONFIG_ZDB_KV_BACKEND_ZMS=y
	 */
	return (void *)db->cfg->kv_backend_fs;
}

static struct zdb_kv_ctx *zdb_kv_ctx_get_or_alloc(zdb_t *db)
{
	struct zdb_kv_ctx *ctx;

	if (db == NULL) {
		return NULL;
	}

	if (db->kv_ctx != NULL) {
		return (struct zdb_kv_ctx *)db->kv_ctx;
	}

	ctx = k_calloc(1U, sizeof(*ctx));
	if (ctx == NULL) {
		return NULL;
	}

	db->kv_ctx = ctx;
	return ctx;
}

static int zdb_kv_ctx_find_entry(const struct zdb_kv_ctx *ctx, const char *namespace_name,
					 const char *key)
{
	size_t i;

	if ((ctx == NULL) || (namespace_name == NULL) || (key == NULL)) {
		return -1;
	}

	for (i = 0U; i < ctx->entry_count; i++) {
		if ((strcmp(ctx->entries[i].namespace_name, namespace_name) == 0) &&
		    (strcmp(ctx->entries[i].key, key) == 0)) {
			return (int)i;
		}
	}

	return -1;
}

static void zdb_kv_ctx_track_set(zdb_t *db, const char *namespace_name, const char *key, uint32_t id)
{
	struct zdb_kv_ctx *ctx;
	int idx;

	if ((db == NULL) || (namespace_name == NULL) || (key == NULL)) {
		return;
	}

	ctx = zdb_kv_ctx_get_or_alloc(db);
	if (ctx == NULL) {
		return;
	}

	idx = zdb_kv_ctx_find_entry(ctx, namespace_name, key);
	if (idx >= 0) {
		ctx->entries[idx].id = id;
		return;
	}

	if (ctx->entry_count >= ZDB_KV_INDEX_MAX_ENTRIES) {
		return;
	}

	idx = (int)ctx->entry_count;
	ctx->entry_count++;
	ctx->entries[idx].id = id;
	(void)strncpy(ctx->entries[idx].namespace_name, namespace_name,
		      sizeof(ctx->entries[idx].namespace_name) - 1U);
	ctx->entries[idx].namespace_name[sizeof(ctx->entries[idx].namespace_name) - 1U] = '\0';
	(void)strncpy(ctx->entries[idx].key, key, sizeof(ctx->entries[idx].key) - 1U);
	ctx->entries[idx].key[sizeof(ctx->entries[idx].key) - 1U] = '\0';
}

static void zdb_kv_ctx_track_delete(zdb_t *db, const char *namespace_name, const char *key)
{
	struct zdb_kv_ctx *ctx;
	int idx;
	size_t i;

	if ((db == NULL) || (namespace_name == NULL) || (key == NULL)) {
		return;
	}

	ctx = (struct zdb_kv_ctx *)db->kv_ctx;
	if ((ctx == NULL) || (ctx->entry_count == 0U)) {
		return;
	}

	idx = zdb_kv_ctx_find_entry(ctx, namespace_name, key);
	if (idx < 0) {
		return;
	}

	for (i = (size_t)idx; (i + 1U) < ctx->entry_count; i++) {
		ctx->entries[i] = ctx->entries[i + 1U];
	}

	ctx->entry_count--;
	(void)memset(&ctx->entries[ctx->entry_count], 0, sizeof(ctx->entries[ctx->entry_count]));
}

static ssize_t zdb_kv_backend_write(zdb_t *db, uint32_t id, const void *value, size_t value_len)
{
#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
	struct nvs_fs *nvs = (struct nvs_fs *)zdb_kv_backend_fs_from_db(db);

	if (nvs == NULL) {
		return -EINVAL;
	}

	return nvs_write(nvs, (uint16_t)id, value, value_len);
#elif defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
	struct zms_fs *zms = (struct zms_fs *)zdb_kv_backend_fs_from_db(db);

	if (zms == NULL) {
		return -EINVAL;
	}

	return zms_write(zms, (zms_id_t)id, value, value_len);
#else
	ARG_UNUSED(db);
	ARG_UNUSED(id);
	ARG_UNUSED(value);
	ARG_UNUSED(value_len);
	return -ENOTSUP;
#endif
}

static ssize_t zdb_kv_backend_read(zdb_t *db, uint32_t id, void *out_value, size_t out_capacity)
{
#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
	struct nvs_fs *nvs = (struct nvs_fs *)zdb_kv_backend_fs_from_db(db);

	if (nvs == NULL) {
		return -EINVAL;
	}

	return nvs_read(nvs, (uint16_t)id, out_value, out_capacity);
#elif defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
	struct zms_fs *zms = (struct zms_fs *)zdb_kv_backend_fs_from_db(db);

	if (zms == NULL) {
		return -EINVAL;
	}

	return zms_read(zms, (zms_id_t)id, out_value, out_capacity);
#else
	ARG_UNUSED(db);
	ARG_UNUSED(id);
	ARG_UNUSED(out_value);
	ARG_UNUSED(out_capacity);
	return -ENOTSUP;
#endif
}

static int zdb_kv_backend_delete(zdb_t *db, uint32_t id)
{
#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
	struct nvs_fs *nvs = (struct nvs_fs *)zdb_kv_backend_fs_from_db(db);

	if (nvs == NULL) {
		return -EINVAL;
	}

	return nvs_delete(nvs, (uint16_t)id);
#elif defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
	struct zms_fs *zms = (struct zms_fs *)zdb_kv_backend_fs_from_db(db);

	if (zms == NULL) {
		return -EINVAL;
	}

	return zms_delete(zms, (zms_id_t)id);
#else
	ARG_UNUSED(db);
	ARG_UNUSED(id);
	return -ENOTSUP;
#endif
}

zdb_status_t zdb_kv_open(zdb_t *db, const char *namespace_name, zdb_kv_t *kv)
{
	void *backend_fs;

	if ((db == NULL) || (namespace_name == NULL) || (kv == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!zdb_key_valid(namespace_name)) {
		return ZDB_ERR_INVAL;
	}

	(void)memset(kv, 0, sizeof(*kv));

	if (db->cfg == NULL) {
		return ZDB_ERR_INVAL;
	}

	backend_fs = zdb_kv_backend_fs_from_db(db);
	if (backend_fs == NULL) {
		return ZDB_ERR_INVAL;
	}

	kv->db = db;
	kv->namespace_name = namespace_name;
	return ZDB_OK;
}

zdb_status_t zdb_kv_close(zdb_kv_t *kv)
{
	if (kv == NULL) {
		return ZDB_ERR_INVAL;
	}

	kv->db = NULL;
	kv->namespace_name = NULL;
	return ZDB_OK;
}

zdb_status_t zdb_kv_set(zdb_kv_t *kv, const char *key, const void *value, size_t value_len)
{
	uint32_t id;
	ssize_t wr = 0;
	ssize_t rd;
	zdb_status_t lock_rc;
	zdb_status_t status;
	uint8_t *io_buf;
	size_t block_size;
	size_t total_len;
	bool slot_matched = false;

	/*
	 * A zero-length value is allowed, as documented; the record still
	 * carries its header, namespace and key, so nothing is ambiguous on
	 * disk. Only a NULL pointer with a non-zero length is a caller error.
	 */
	if ((kv == NULL) || (kv->db == NULL) || (kv->namespace_name == NULL) ||
	    ((value == NULL) && (value_len > 0U)) || !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	if (kv->db->kv_io_slab == NULL) {
		return ZDB_ERR_INVAL;
	}

	block_size = kv->db->kv_io_slab->info.block_size;
	total_len = ZDB_KV_REC_HDR_SIZE + strlen(kv->namespace_name) + strlen(key) +
		    value_len;

	if (total_len > block_size) {
		return ZDB_ERR_NOMEM;
	}

	if (k_mem_slab_alloc(kv->db->kv_io_slab, (void **)&io_buf, K_NO_WAIT) != 0) {
		return ZDB_ERR_NOMEM;
	}

	id = zdb_kv_record_id(kv->namespace_name, key);
	lock_rc = zdb_lock_write(kv->db);
	if (lock_rc != ZDB_OK) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		return lock_rc;
	}

	/*
	 * Pre-read the slot: a valid record for a different (namespace, key)
	 * means an ID collision, which must not overwrite the stored record.
	 * Anything else (empty slot, old-format record, garbage) is claimed.
	 */
	rd = zdb_kv_backend_read(kv->db, id, io_buf, block_size);
	if (rd >= 0) {
		size_t usable = ((size_t)rd < block_size) ? (size_t)rd : block_size;

		switch (zdb_kv_record_check(io_buf, usable, kv->namespace_name, key,
					    NULL)) {
		case ZDB_KV_REC_MISMATCH:
			zdb_unlock_write(kv->db);
			k_mem_slab_free(kv->db->kv_io_slab, io_buf);
			status = ZDB_ERR_COLLISION;
			goto emit;
		case ZDB_KV_REC_MATCH:
			slot_matched = true;
			break;
		case ZDB_KV_REC_FOREIGN:
		default:
			break;
		}
	}

	(void)zdb_kv_record_build(io_buf, kv->namespace_name, key, value, value_len);

	wr = zdb_kv_backend_write(kv->db, id, io_buf, total_len);
	if (wr < 0) {
		status = zdb_status_from_errno((int)wr);
	} else if (((size_t)wr == total_len) || ((wr == 0) && slot_matched)) {
		/*
		 * Backends report 0 written when the stored bytes are already
		 * identical (no flash write needed); with a matching pre-read
		 * that is a successful set.
		 */
		status = ZDB_OK;
	} else {
		status = ZDB_ERR_IO;
	}

	if (status == ZDB_OK) {
		zdb_kv_ctx_track_set(kv->db, kv->namespace_name, key, id);
	}
	zdb_unlock_write(kv->db);
	k_mem_slab_free(kv->db->kv_io_slab, io_buf);

emit:
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_kv_event(kv->db, ZDB_EVENT_KV_SET, kv->namespace_name, key, value_len, status);
#endif

	return status;
}

zdb_status_t zdb_kv_get(zdb_kv_t *kv, const char *key, void *out_value,
			size_t out_capacity, size_t *out_len)
{
	uint32_t id;
	ssize_t rd;
	zdb_status_t lock_rc;
	uint8_t *io_buf;
	size_t block_size;
	size_t usable;
	size_t value_off = 0U;
	size_t value_len;

	if ((kv == NULL) || (kv->db == NULL) || (kv->namespace_name == NULL) ||
	    (out_len == NULL) || !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if ((out_value == NULL) && (out_capacity > 0U)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	if (kv->db->kv_io_slab == NULL) {
		return ZDB_ERR_INVAL;
	}

	block_size = kv->db->kv_io_slab->info.block_size;

	if (k_mem_slab_alloc(kv->db->kv_io_slab, (void **)&io_buf, K_NO_WAIT) != 0) {
		return ZDB_ERR_NOMEM;
	}

	id = zdb_kv_record_id(kv->namespace_name, key);
	lock_rc = zdb_lock_read(kv->db);
	if (lock_rc != ZDB_OK) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		return lock_rc;
	}

	rd = zdb_kv_backend_read(kv->db, id, io_buf, block_size);
	zdb_unlock_read(kv->db);

	if (rd < 0) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		*out_len = 0U;
		return zdb_status_from_errno((int)rd);
	}

	usable = ((size_t)rd < block_size) ? (size_t)rd : block_size;

	if (zdb_kv_record_check(io_buf, usable, kv->namespace_name, key,
				&value_off) != ZDB_KV_REC_MATCH) {
		/* ID alias of another record, or an old-format record. */
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		*out_len = 0U;
		return ZDB_ERR_NOT_FOUND;
	}

	/* Backends report the full stored length even beyond our buffer. */
	value_len = (size_t)rd - value_off;
	*out_len = value_len;

	if ((out_value != NULL) && (out_capacity > 0U)) {
		size_t available = usable - value_off;
		size_t copy_len = (value_len < out_capacity) ? value_len : out_capacity;

		if (copy_len > available) {
			copy_len = available;
		}
		(void)memcpy(out_value, &io_buf[value_off], copy_len);
	}

	k_mem_slab_free(kv->db->kv_io_slab, io_buf);
	return ZDB_OK;
}

zdb_status_t zdb_kv_delete(zdb_kv_t *kv, const char *key)
{
	uint32_t id;
	int rc;
	ssize_t rd;
	zdb_status_t lock_rc;
	zdb_status_t status;
	uint8_t *io_buf = NULL;
	size_t block_size;
	size_t usable;
	bool lock_held = false;

	if ((kv == NULL) || (kv->db == NULL) || (kv->namespace_name == NULL) ||
	    !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	if (kv->db->kv_io_slab == NULL) {
		return ZDB_ERR_INVAL;
	}

	block_size = kv->db->kv_io_slab->info.block_size;

	if (k_mem_slab_alloc(kv->db->kv_io_slab, (void **)&io_buf, K_NO_WAIT) != 0) {
		status = ZDB_ERR_NOMEM;
		goto out;
	}

	id = zdb_kv_record_id(kv->namespace_name, key);
	lock_rc = zdb_lock_write(kv->db);
	if (lock_rc != ZDB_OK) {
		status = lock_rc;
		goto out;
	}
	lock_held = true;

	/* Only delete a slot that verifiably belongs to this (namespace, key). */
	rd = zdb_kv_backend_read(kv->db, id, io_buf, block_size);
	if (rd < 0) {
		status = zdb_status_from_errno((int)rd);
		goto out;
	}

	usable = ((size_t)rd < block_size) ? (size_t)rd : block_size;

	if (zdb_kv_record_check(io_buf, usable, kv->namespace_name, key,
				NULL) != ZDB_KV_REC_MATCH) {
		status = ZDB_ERR_NOT_FOUND;
		goto out;
	}

	rc = zdb_kv_backend_delete(kv->db, id);
	if (rc >= 0) {
		zdb_kv_ctx_track_delete(kv->db, kv->namespace_name, key);
		status = ZDB_OK;
	} else {
		status = zdb_status_from_errno(rc);
	}

out:
	if (lock_held) {
		zdb_unlock_write(kv->db);
	}
	if (io_buf != NULL) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
	}

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_kv_event(kv->db, ZDB_EVENT_KV_DELETE, kv->namespace_name, key, 0U, status);
#endif

	return status;
}

zdb_status_t zdb_kv_iter_open(zdb_kv_t *kv, zdb_kv_iter_t *out_iter)
{
	struct zdb_kv_ctx *ctx;

	if ((kv == NULL) || (kv->db == NULL) || (out_iter == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_kv_ctx_get_or_alloc(kv->db);

	(void)memset(out_iter, 0, sizeof(*out_iter));
	out_iter->kv = kv;
	out_iter->position = 0U;
	return ZDB_OK;
}

zdb_status_t zdb_kv_iter_next(zdb_kv_iter_t *iter, char *out_key,
			      size_t out_key_capacity, size_t *out_key_len,
			      void *out_value, size_t out_value_capacity,
			      size_t *out_value_len)
{
	zdb_t *db;
	struct zdb_kv_ctx *ctx;
	zdb_status_t lock_rc;
	char key_local[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	size_t i;

	if ((iter == NULL) || (iter->kv == NULL) || (iter->kv->db == NULL) ||
	    (out_key == NULL) || (out_key_len == NULL) || (out_value_len == NULL) ||
	    (out_key_capacity == 0U)) {
		return ZDB_ERR_INVAL;
	}

	if ((out_value == NULL) && (out_value_capacity > 0U)) {
		return ZDB_ERR_INVAL;
	}

	db = iter->kv->db;

	while (true) {
		key_local[0] = '\0';

		lock_rc = zdb_lock_read(db);
		if (lock_rc != ZDB_OK) {
			return lock_rc;
		}

		ctx = (struct zdb_kv_ctx *)db->kv_ctx;
		if ((ctx == NULL) || (iter->position >= ctx->entry_count)) {
			zdb_unlock_read(db);
			*out_key_len = 0U;
			*out_value_len = 0U;
			return ZDB_ERR_NOT_FOUND;
		}

		for (i = iter->position; i < ctx->entry_count; i++) {
			if (strcmp(ctx->entries[i].namespace_name, iter->kv->namespace_name) == 0) {
				(void)strncpy(key_local, ctx->entries[i].key, sizeof(key_local) - 1U);
				key_local[sizeof(key_local) - 1U] = '\0';
				iter->position = i + 1U;
				break;
			}
		}

		if (i >= ctx->entry_count) {
			iter->position = ctx->entry_count;
		}

		zdb_unlock_read(db);

		if (key_local[0] == '\0') {
			*out_key_len = 0U;
			*out_value_len = 0U;
			return ZDB_ERR_NOT_FOUND;
		}

		*out_key_len = strlen(key_local);
		if ((*out_key_len + 1U) > out_key_capacity) {
			*out_value_len = 0U;
			return ZDB_ERR_NOMEM;
		}

		(void)strcpy(out_key, key_local);
		lock_rc = zdb_kv_get(iter->kv, key_local, out_value, out_value_capacity, out_value_len);
		if (lock_rc == ZDB_OK) {
			return ZDB_OK;
		}

		/* Skip stale entries that no longer exist in backend and keep iterating. */
		if (lock_rc != ZDB_ERR_NOT_FOUND) {
			*out_key_len = 0U;
			*out_value_len = 0U;
			return lock_rc;
		}
	}
}

zdb_status_t zdb_kv_iter_close(zdb_kv_iter_t *iter)
{
	if (iter == NULL) {
		return ZDB_ERR_INVAL;
	}

	iter->kv = NULL;
	iter->position = 0U;
	iter->impl = NULL;
	return ZDB_OK;
}

zdb_status_t zdb_kv_defaults_apply_ns(zdb_t *db, const char *namespace_filter)
{
	zdb_status_t first_error = ZDB_OK;
	size_t i;

	if ((db == NULL) || (db->cfg == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->cfg->kv_defaults == NULL) || (db->cfg->kv_default_count == 0U)) {
		return ZDB_OK;
	}

	if (zdb_kv_backend_fs_from_db(db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	for (i = 0U; i < db->cfg->kv_default_count; i++) {
		const zdb_kv_default_t *def = &db->cfg->kv_defaults[i];
		zdb_kv_t kv;
		zdb_status_t rc;
		size_t existing_len = 0U;

		if ((def->namespace_name == NULL) || (def->key == NULL)) {
			if (first_error == ZDB_OK) {
				first_error = ZDB_ERR_INVAL;
			}
			continue;
		}

		if ((namespace_filter != NULL) &&
		    (strcmp(def->namespace_name, namespace_filter) != 0)) {
			continue;
		}

		rc = zdb_kv_open(db, def->namespace_name, &kv);
		if (rc != ZDB_OK) {
			if (first_error == ZDB_OK) {
				first_error = rc;
			}
			continue;
		}

		/*
		 * Write-if-missing. A key that is already present keeps whatever
		 * value it holds, which is what makes this safe to re-run after a
		 * firmware update: new entries appear, changed values survive.
		 */
		rc = zdb_kv_get(&kv, def->key, NULL, 0U, &existing_len);
		if (rc == ZDB_ERR_NOT_FOUND) {
			rc = zdb_kv_set(&kv, def->key, def->value, def->value_len);
		}

		if ((rc != ZDB_OK) && (first_error == ZDB_OK)) {
			first_error = rc;
		}

		(void)zdb_kv_close(&kv);
	}

	return first_error;
}

zdb_status_t zdb_kv_defaults_apply(zdb_t *db)
{
	return zdb_kv_defaults_apply_ns(db, NULL);
}

zdb_status_t zdb_kv_set_str(zdb_kv_t *kv, const char *key, const char *value)
{
	if (value == NULL) {
		return ZDB_ERR_INVAL;
	}

	/* Store the terminator so readers can hand the bytes straight back. */
	return zdb_kv_set(kv, key, value, strlen(value) + 1U);
}

zdb_status_t zdb_kv_get_str(zdb_kv_t *kv, const char *key, char *out_str,
			    size_t out_capacity, size_t *out_len)
{
	size_t stored_len = 0U;
	const void *terminator;
	size_t copied;
	zdb_status_t rc;

	if ((out_str == NULL) || (out_capacity == 0U) || (out_len == NULL)) {
		return ZDB_ERR_INVAL;
	}

	/*
	 * Reserve the last byte for the terminator so the result is always a
	 * valid C string, even when the stored value is longer or was written
	 * as a raw blob without one.
	 */
	rc = zdb_kv_get(kv, key, out_str, out_capacity - 1U, &stored_len);
	if (rc != ZDB_OK) {
		*out_len = 0U;
		out_str[0] = '\0';
		return rc;
	}

	copied = (stored_len < (out_capacity - 1U)) ? stored_len : (out_capacity - 1U);
	terminator = memchr(out_str, '\0', copied);

	if (terminator != NULL) {
		/* Whole string present: its length is where the NUL sits. */
		*out_len = (size_t)((const char *)terminator - out_str);
	} else {
		/*
		 * Either the value did not fit, or it was written through the
		 * raw API without a terminator. Terminate what we copied and
		 * report the stored size, which lets the caller detect the
		 * truncation via *out_len + 1 > out_capacity.
		 */
		out_str[copied] = '\0';
		*out_len = stored_len;
	}

	return ZDB_OK;
}

#endif /* CONFIG_ZDB_KV */
