/* KV module implementation */
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)

#include "zephyrdb_internal.h"

#include <errno.h>
#include <string.h>

#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
#include <zephyr/fs/nvs.h>
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
	bool hydrated;
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
 * On-disk KV entry format (v2, with collision detection):
 *   [key_len: 1 byte] [key: key_len bytes] [value: remaining bytes]
 *
 * key_len is the string length excluding NUL terminator.
 * Total stored size = 1 + key_len + value_len.
 */
#define ZDB_KV_HEADER_SIZE(key) (1U + strlen(key))

#if defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
static uint32_t zdb_fnv1a32(const char *s)
{
	uint32_t hash = 0x811C9DC5u;

	while ((*s) != '\0') {
		hash ^= (uint8_t)(*s);
		hash *= 0x01000193u;
		s++;
	}

	return hash;
}
#endif

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

static uint32_t zdb_kv_key_to_id(const char *key);
static void zdb_kv_hydrate_from_backend(zdb_t *db, const char *namespace_name);

static uint16_t __unused zdb_fnv1a16(const char *s)
{
	uint32_t hash = 0x811C9DC5u;

	while ((*s) != '\0') {
		hash ^= (uint8_t)(*s);
		hash *= 0x01000193u;
		s++;
	}

	/* Avoid returning 0 which can be reserved by some backends. */
	hash = (hash & 0xFFFFu);
	if (hash == 0U) {
		hash = 1U;
	}

	return (uint16_t)hash;
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
	ssize_t wr;
	zdb_status_t lock_rc;
	zdb_status_t status;
	uint8_t *io_buf;
	size_t key_len;
	size_t total_len;

	if ((kv == NULL) || (kv->db == NULL) || (value == NULL) || (value_len == 0U) ||
	    !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	if (kv->db->kv_io_slab == NULL) {
		return ZDB_ERR_INVAL;
	}

	key_len = strlen(key);
	total_len = 1U + key_len + value_len;

	if (total_len > kv->db->kv_io_slab->info.block_size) {
		return ZDB_ERR_NOMEM;
	}

	if (k_mem_slab_alloc(kv->db->kv_io_slab, (void **)&io_buf, K_NO_WAIT) != 0) {
		return ZDB_ERR_NOMEM;
	}

	/* Build wrapped entry: [key_len][key][value] */
	io_buf[0] = (uint8_t)key_len;
	(void)memcpy(&io_buf[1], key, key_len);
	(void)memcpy(&io_buf[1 + key_len], value, value_len);

	id = zdb_kv_key_to_id(key);
	lock_rc = zdb_lock_write(kv->db);
	if (lock_rc != ZDB_OK) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		return lock_rc;
	}

	wr = zdb_kv_backend_write(kv->db, id, io_buf, total_len);
	if ((wr >= 0) && ((size_t)wr == total_len)) {
		zdb_kv_ctx_track_set(kv->db, kv->namespace_name, key, id);
	}
	zdb_unlock_write(kv->db);
	k_mem_slab_free(kv->db->kv_io_slab, io_buf);

	if (wr < 0) {
		status = zdb_status_from_errno((int)wr);
	} else if ((size_t)wr != total_len) {
		status = ZDB_ERR_IO;
	} else {
		status = ZDB_OK;
	}

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
	size_t key_len;
	size_t stored_key_len;
	size_t value_len;

	if ((kv == NULL) || (kv->db == NULL) || (out_len == NULL) || !zdb_key_valid(key)) {
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

	if (k_mem_slab_alloc(kv->db->kv_io_slab, (void **)&io_buf, K_NO_WAIT) != 0) {
		return ZDB_ERR_NOMEM;
	}

	id = zdb_kv_key_to_id(key);
	lock_rc = zdb_lock_read(kv->db);
	if (lock_rc != ZDB_OK) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		return lock_rc;
	}

	rd = zdb_kv_backend_read(kv->db, id, io_buf, kv->db->kv_io_slab->info.block_size);
	zdb_unlock_read(kv->db);

	if (rd < 0) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		*out_len = 0U;
		return zdb_status_from_errno((int)rd);
	}

	/* Verify key prefix: [key_len:1][key:key_len][value:...] */
	if ((size_t)rd < 1U) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		*out_len = 0U;
		return ZDB_ERR_CORRUPT;
	}

	stored_key_len = (size_t)io_buf[0];
	key_len = strlen(key);

	if (stored_key_len != key_len || (size_t)rd < (1U + stored_key_len)) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		*out_len = 0U;
		return ZDB_ERR_NOT_FOUND;
	}

	if (memcmp(&io_buf[1], key, key_len) != 0) {
		/* Hash collision: stored key does not match requested key */
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		*out_len = 0U;
		return ZDB_ERR_NOT_FOUND;
	}

	value_len = (size_t)rd - 1U - stored_key_len;
	*out_len = value_len;

	if ((out_value != NULL) && (out_capacity > 0U)) {
		size_t copy_len = (value_len < out_capacity) ? value_len : out_capacity;

		(void)memcpy(out_value, &io_buf[1 + stored_key_len], copy_len);
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
	uint8_t *io_buf;
	size_t key_len;
	size_t stored_key_len;

	if ((kv == NULL) || (kv->db == NULL) || !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	if (kv->db->kv_io_slab == NULL) {
		return ZDB_ERR_INVAL;
	}

	if (k_mem_slab_alloc(kv->db->kv_io_slab, (void **)&io_buf, K_NO_WAIT) != 0) {
		return ZDB_ERR_NOMEM;
	}

	id = zdb_kv_key_to_id(key);
	key_len = strlen(key);
	lock_rc = zdb_lock_write(kv->db);
	if (lock_rc != ZDB_OK) {
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		return lock_rc;
	}

	/* Read existing entry to verify key matches before deleting */
	rd = zdb_kv_backend_read(kv->db, id, io_buf, kv->db->kv_io_slab->info.block_size);
	if (rd < 0) {
		zdb_unlock_write(kv->db);
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		return zdb_status_from_errno((int)rd);
	}

	stored_key_len = (size_t)io_buf[0];
	if ((size_t)rd < (1U + stored_key_len) || stored_key_len != key_len ||
	    memcmp(&io_buf[1], key, key_len) != 0) {
		zdb_unlock_write(kv->db);
		k_mem_slab_free(kv->db->kv_io_slab, io_buf);
		return ZDB_ERR_NOT_FOUND;
	}

	rc = zdb_kv_backend_delete(kv->db, id);
	if (rc >= 0) {
		zdb_kv_ctx_track_delete(kv->db, kv->namespace_name, key);
	}
	zdb_unlock_write(kv->db);
	k_mem_slab_free(kv->db->kv_io_slab, io_buf);

	if (rc < 0) {
		status = zdb_status_from_errno(rc);
	} else {
		status = ZDB_OK;
	}

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_kv_event(kv->db, ZDB_EVENT_KV_DELETE, kv->namespace_name, key, 0U, status);
#endif

	return status;
}

static uint32_t zdb_kv_key_to_id(const char *key)
{
	uint32_t id;

	if (key == NULL) {
		return 1U;
	}

#if defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
	id = zdb_fnv1a32(key);
#else
	id = (uint32_t)zdb_fnv1a16(key);
#endif

	if (id == 0U) {
		id = 1U;
	}

	return id;
}

/*
 * Hydration from persisted v2 KV records is intentionally disabled.
 *
 * The in-RAM index is namespace-aware, but the persisted v2 record format does
 * not encode namespace information.  Rebuilding the index during startup would
 * therefore misattribute recovered keys to whichever namespace first triggers
 * hydration, breaking correct cross-session namespace iteration.
 *
 * Until namespace metadata is persisted as part of the backend record (and
 * hydration can reconstruct it), skip backend hydration entirely rather than
 * populating an incorrect index.
 */
static void zdb_kv_hydrate_from_backend(zdb_t *db, const char *namespace_name)
{
	(void)db;
	(void)namespace_name;
}

#if 0 /* disabled: namespace not encoded in on-disk format */
static void zdb_kv_hydrate_from_backend_nvs(zdb_t *db, const char *namespace_name)
{
#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
	struct nvs_fs *nvs;
	struct zdb_kv_ctx *ctx;
	uint8_t *io_buf = NULL;
	size_t block_size;
	uint16_t scan_id;

	if ((db == NULL) || (db->kv_io_slab == NULL)) {
		return;
	}

	nvs = (struct nvs_fs *)zdb_kv_backend_fs_from_db(db);
	if (nvs == NULL) {
		return;
	}

	ctx = zdb_kv_ctx_get_or_alloc(db);
	if (ctx == NULL) {
		return;
	}

	block_size = db->kv_io_slab->info.block_size;
	if (k_mem_slab_alloc(db->kv_io_slab, (void **)&io_buf, K_NO_WAIT) != 0) {
		return;
	}

	for (scan_id = 1U; scan_id != 0U; scan_id++) {
		ssize_t rd;
		size_t stored_key_len;
		char key_buf[CONFIG_ZDB_MAX_KEY_LEN + 1U];
		uint32_t verify_id;

		if (ctx->entry_count >= ZDB_KV_INDEX_MAX_ENTRIES) {
			break;
		}

		rd = nvs_read(nvs, scan_id, io_buf, block_size);
		if (rd <= 0) {
			continue;
		}

		/* Validate v2 entry format: [key_len:1][key][value] */
		stored_key_len = (size_t)io_buf[0];
		if ((stored_key_len == 0U) ||
		    (stored_key_len > (size_t)CONFIG_ZDB_MAX_KEY_LEN) ||
		    ((size_t)rd < (1U + stored_key_len))) {
			continue;
		}

		(void)memcpy(key_buf, &io_buf[1], stored_key_len);
		key_buf[stored_key_len] = '\0';

		/* Verify extracted key hashes to this ID (proves ZephyrDB ownership) */
		verify_id = (uint32_t)zdb_fnv1a16(key_buf);
		if (verify_id == 0U) {
			verify_id = 1U;
		}
		if (verify_id != (uint32_t)scan_id) {
			continue;
		}

		/* Skip if already tracked (e.g. from a runtime set) */
		if (zdb_kv_ctx_find_entry(ctx, namespace_name, key_buf) >= 0) {
			continue;
		}

		zdb_kv_ctx_track_set(db, namespace_name, key_buf, (uint32_t)scan_id);
	}

	k_mem_slab_free(db->kv_io_slab, io_buf);
#else
	/* ZMS 32-bit ID space is too large for brute-force scan */
	ARG_UNUSED(db);
	ARG_UNUSED(namespace_name);
#endif
}
#endif /* disabled hydration */

zdb_status_t zdb_kv_iter_open(zdb_kv_t *kv, zdb_kv_iter_t *out_iter)
{
	struct zdb_kv_ctx *ctx;

	if ((kv == NULL) || (kv->db == NULL) || (out_iter == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_kv_ctx_get_or_alloc(kv->db);
	if (ctx != NULL) {
		zdb_lock_write(kv->db);
		if (!ctx->hydrated) {
			zdb_kv_hydrate_from_backend(kv->db, kv->namespace_name);
			ctx->hydrated = true;
		}
		zdb_unlock_write(kv->db);
	}

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

#endif /* CONFIG_ZDB_KV */
