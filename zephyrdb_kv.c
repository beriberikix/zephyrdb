/* KV module implementation */
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)

#include "zephyrdb_internal.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>

#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
#include <zephyr/kvss/nvs.h>
#endif
#if defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
#include <zephyr/kvss/zms.h>
#endif

#ifndef CONFIG_ZDB_KV_INDEX_MAX_ENTRIES
#define CONFIG_ZDB_KV_INDEX_MAX_ENTRIES 128
#endif

#define ZDB_KV_INDEX_MAX_ENTRIES ((size_t)CONFIG_ZDB_KV_INDEX_MAX_ENTRIES)

#if defined(CONFIG_ZDB_KV_PERSIST_INDEX) && (CONFIG_ZDB_KV_PERSIST_INDEX)
#define ZDB_KV_PERSIST_INDEX 1
#else
#define ZDB_KV_PERSIST_INDEX 0
#endif

/*
 * Persisted key index.
 *
 * Enumerating keys by scanning the backend's ID space is not an option: NVS
 * would need 65536 probes and ZMS 2^32. Instead the set of in-use record IDs
 * is stored in one reserved record, and the (namespace, key) pair for each ID
 * comes from the v2 record itself, which already carries both. Rebuilding is
 * therefore one read per tracked key.
 *
 * On-disk index format (v1):
 *   [tag: 0xD1] [version: 1] [count: 2 bytes LE] [count x 4-byte LE record ID]
 *
 * IDs are stored 4-byte wide on both backends so the format does not depend
 * on the backend's ID width.
 */
#define ZDB_KV_INDEX_TAG      0xD1u
#define ZDB_KV_INDEX_VERSION  1u
#define ZDB_KV_INDEX_HDR_SIZE 4U
#define ZDB_KV_INDEX_MAX_BYTES (ZDB_KV_INDEX_HDR_SIZE + (4U * ZDB_KV_INDEX_MAX_ENTRIES))

/*
 * Record ID reserved for the index itself: the top of each backend's ID space,
 * which no key can occupy because zdb_kv_record_id() remaps a hash that lands
 * there.
 */
#if defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
#define ZDB_KV_INDEX_REC_ID 0xFFFFFFFFu
#else
#define ZDB_KV_INDEX_REC_ID 0x0000FFFFu
#endif

struct zdb_kv_index_entry {
	char namespace_name[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	uint32_t id;
};

struct zdb_kv_ctx {
	struct zdb_kv_index_entry entries[ZDB_KV_INDEX_MAX_ENTRIES];
	size_t entry_count;
	bool loaded;
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

	/*
	 * Avoid ID 0, which backends may reserve, and the ID that holds the
	 * key index. Remapping costs a key its natural slot with probability
	 * 2^-16 (NVS) or 2^-32 (ZMS).
	 */
	if ((hash == 0U) || (hash == ZDB_KV_INDEX_REC_ID)) {
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

static ssize_t zdb_kv_backend_write(zdb_t *db, uint32_t id, const void *value, size_t value_len);
static ssize_t zdb_kv_backend_read(zdb_t *db, uint32_t id, void *out_value, size_t out_capacity);

#if ZDB_KV_PERSIST_INDEX
static void zdb_kv_index_store(zdb_t *db, struct zdb_kv_ctx *ctx);

/*
 * Rebuild the in-RAM index from the reserved record.
 *
 * Each stored ID is read back and its (namespace, key) recovered from the v2
 * record header. IDs whose record is gone — a crash between the index write
 * and the data write, or a delete that did not get to rewrite the index — are
 * dropped, and the pruned index is written back so the next boot is clean.
 */
static void zdb_kv_index_load(zdb_t *db, struct zdb_kv_ctx *ctx)
{
	uint8_t index_buf[ZDB_KV_INDEX_MAX_BYTES];
	uint8_t *rec_buf;
	size_t block_size;
	size_t stored_count;
	size_t i;
	ssize_t rd;
	bool pruned = false;

	if (zdb_kv_backend_fs_from_db(db) == NULL) {
		return;
	}

	rd = zdb_kv_backend_read(db, ZDB_KV_INDEX_REC_ID, index_buf, sizeof(index_buf));
	if (rd < (ssize_t)ZDB_KV_INDEX_HDR_SIZE) {
		return;
	}

	if ((index_buf[0] != (uint8_t)ZDB_KV_INDEX_TAG) ||
	    (index_buf[1] != (uint8_t)ZDB_KV_INDEX_VERSION)) {
		return;
	}

	stored_count = (size_t)sys_get_le16(&index_buf[2]);
	if (stored_count > ZDB_KV_INDEX_MAX_ENTRIES) {
		stored_count = ZDB_KV_INDEX_MAX_ENTRIES;
	}
	if ((ZDB_KV_INDEX_HDR_SIZE + (4U * stored_count)) > (size_t)rd) {
		return;
	}

	if (db->kv_io_slab == NULL) {
		return;
	}
	block_size = db->kv_io_slab->info.block_size;
	if (k_mem_slab_alloc(db->kv_io_slab, (void **)&rec_buf, K_NO_WAIT) != 0) {
		return;
	}

	for (i = 0U; i < stored_count; i++) {
		uint32_t id = sys_get_le32(&index_buf[ZDB_KV_INDEX_HDR_SIZE + (4U * i)]);
		size_t ns_len;
		size_t key_len;
		struct zdb_kv_index_entry *entry;

		rd = zdb_kv_backend_read(db, id, rec_buf, block_size);
		if (rd < (ssize_t)ZDB_KV_REC_HDR_SIZE) {
			pruned = true;
			continue;
		}

		ns_len = rec_buf[1];
		key_len = rec_buf[2];
		if ((rec_buf[0] != (uint8_t)ZDB_KV_REC_TAG) || (ns_len == 0U) || (key_len == 0U) ||
		    (ns_len > (size_t)CONFIG_ZDB_MAX_KEY_LEN) ||
		    (key_len > (size_t)CONFIG_ZDB_MAX_KEY_LEN) ||
		    ((ZDB_KV_REC_HDR_SIZE + ns_len + key_len) > (size_t)rd)) {
			pruned = true;
			continue;
		}

		entry = &ctx->entries[ctx->entry_count];
		(void)memcpy(entry->namespace_name, &rec_buf[ZDB_KV_REC_HDR_SIZE], ns_len);
		entry->namespace_name[ns_len] = '\0';
		(void)memcpy(entry->key, &rec_buf[ZDB_KV_REC_HDR_SIZE + ns_len], key_len);
		entry->key[key_len] = '\0';
		entry->id = id;
		ctx->entry_count++;
	}

	k_mem_slab_free(db->kv_io_slab, rec_buf);

	if (pruned) {
		zdb_kv_index_store(db, ctx);
	}
}

/* Write the current index to its reserved record. Best effort: a failure here
 * costs iteration coverage, not stored data.
 */
static void zdb_kv_index_store(zdb_t *db, struct zdb_kv_ctx *ctx)
{
	uint8_t index_buf[ZDB_KV_INDEX_MAX_BYTES];
	size_t i;
	size_t len;

	if (zdb_kv_backend_fs_from_db(db) == NULL) {
		return;
	}

	index_buf[0] = (uint8_t)ZDB_KV_INDEX_TAG;
	index_buf[1] = (uint8_t)ZDB_KV_INDEX_VERSION;
	sys_put_le16((uint16_t)ctx->entry_count, &index_buf[2]);

	for (i = 0U; i < ctx->entry_count; i++) {
		sys_put_le32(ctx->entries[i].id, &index_buf[ZDB_KV_INDEX_HDR_SIZE + (4U * i)]);
	}

	len = ZDB_KV_INDEX_HDR_SIZE + (4U * ctx->entry_count);
	(void)zdb_kv_backend_write(db, ZDB_KV_INDEX_REC_ID, index_buf, len);
}
#endif /* ZDB_KV_PERSIST_INDEX */

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

#if ZDB_KV_PERSIST_INDEX
	/*
	 * Populate from storage on first use, so iteration sees keys written
	 * before this boot. Callers hold the instance lock, or are the only
	 * user of the instance at this point.
	 */
	zdb_kv_index_load(db, ctx);
#endif
	ctx->loaded = true;

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

/* Returns false when the key was stored but could not be indexed. */
static bool zdb_kv_ctx_track_set(zdb_t *db, const char *namespace_name, const char *key,
				 uint32_t id)
{
	struct zdb_kv_ctx *ctx;
	int idx;

	if ((db == NULL) || (namespace_name == NULL) || (key == NULL)) {
		return true;
	}

	ctx = zdb_kv_ctx_get_or_alloc(db);
	if (ctx == NULL) {
		return true;
	}

	idx = zdb_kv_ctx_find_entry(ctx, namespace_name, key);
	if (idx >= 0) {
		ctx->entries[idx].id = id;
		return true;
	}

	if (ctx->entry_count >= ZDB_KV_INDEX_MAX_ENTRIES) {
		/*
		 * The key is still stored and readable, it just cannot be
		 * enumerated. Raise CONFIG_ZDB_KV_INDEX_MAX_ENTRIES if a
		 * deployment needs every key iterable. Reported to the caller
		 * so it can announce the limit once it has unlocked.
		 */
		return false;
	}

	idx = (int)ctx->entry_count;
	ctx->entry_count++;
	ctx->entries[idx].id = id;
	(void)strncpy(ctx->entries[idx].namespace_name, namespace_name,
		      sizeof(ctx->entries[idx].namespace_name) - 1U);
	ctx->entries[idx].namespace_name[sizeof(ctx->entries[idx].namespace_name) - 1U] = '\0';
	(void)strncpy(ctx->entries[idx].key, key, sizeof(ctx->entries[idx].key) - 1U);
	ctx->entries[idx].key[sizeof(ctx->entries[idx].key) - 1U] = '\0';

#if ZDB_KV_PERSIST_INDEX
	/* Only a new key changes the stored set; overwrites cost no extra write. */
	zdb_kv_index_store(db, ctx);
#endif
	return true;
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

#if ZDB_KV_PERSIST_INDEX
	zdb_kv_index_store(db, ctx);
#endif
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
	bool indexed = true;

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

	/*
	 * Record the key in the index before the data write. A crash in
	 * between then leaves an index entry whose record is missing, which
	 * iteration skips and the next rebuild prunes. The reverse order would
	 * leave a stored key that nothing knows how to enumerate.
	 */
	indexed = zdb_kv_ctx_track_set(kv->db, kv->namespace_name, key, id);

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

	if (status != ZDB_OK) {
		/* The data never landed; drop the entry we optimistically added. */
		zdb_kv_ctx_track_delete(kv->db, kv->namespace_name, key);
	}
	zdb_unlock_write(kv->db);
	k_mem_slab_free(kv->db->kv_io_slab, io_buf);

emit:
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_kv_event(kv->db, ZDB_EVENT_KV_SET, kv->namespace_name, key, value_len, status);

	/*
	 * The value landed but nothing can enumerate it. Announced separately
	 * from the set so a listener sees both what was stored and that the
	 * index has stopped keeping up.
	 */
	if (!indexed && (status == ZDB_OK)) {
		zdb_emit_kv_event(kv->db, ZDB_EVENT_KV_INDEX_FULL, kv->namespace_name, key,
				  value_len, ZDB_OK);
	}
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

	/*
	 * The key index is heap-allocated and sized by
	 * CONFIG_ZDB_KV_INDEX_MAX_ENTRIES, so a small CONFIG_HEAP_MEM_POOL_SIZE
	 * can leave it unavailable. Report that instead of opening an iterator
	 * that would report an empty namespace no matter what it holds.
	 */
	ctx = zdb_kv_ctx_get_or_alloc(kv->db);
	if (ctx == NULL) {
		return ZDB_ERR_NOMEM;
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

/*
 * Find the first indexed key of a namespace at or after @p from_pos.
 *
 * Copies the key out under the read lock so the caller can delete it without
 * holding the instance lock (zdb_kv_delete() takes the write lock itself).
 * Returns the entry position, or -1 when the namespace has no more keys.
 */
static int zdb_kv_next_key_in_ns(zdb_t *db, const char *namespace_name, size_t from_pos,
				 char *out_key, size_t out_key_size)
{
	const struct zdb_kv_ctx *ctx;
	int found = -1;
	size_t i;

	if (zdb_lock_read(db) != ZDB_OK) {
		return -1;
	}

	ctx = (const struct zdb_kv_ctx *)db->kv_ctx;
	if (ctx != NULL) {
		for (i = from_pos; i < ctx->entry_count; i++) {
			if (strcmp(ctx->entries[i].namespace_name, namespace_name) != 0) {
				continue;
			}

			(void)strncpy(out_key, ctx->entries[i].key, out_key_size - 1U);
			out_key[out_key_size - 1U] = '\0';
			found = (int)i;
			break;
		}
	}

	zdb_unlock_read(db);
	return found;
}

zdb_status_t zdb_kv_reset_namespace(zdb_kv_t *kv)
{
	char key[CONFIG_ZDB_MAX_KEY_LEN + 1U];
	zdb_status_t first_error = ZDB_OK;
	zdb_status_t rc;
	size_t pos = 0U;

	if ((kv == NULL) || (kv->db == NULL) || (kv->namespace_name == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	/* Populate the index before walking it, so keys from earlier boots are
	 * included rather than silently surviving the reset.
	 */
	if (zdb_kv_ctx_get_or_alloc(kv->db) == NULL) {
		return ZDB_ERR_NOMEM;
	}

	for (;;) {
		int entry_pos = zdb_kv_next_key_in_ns(kv->db, kv->namespace_name, pos, key,
						      sizeof(key));

		if (entry_pos < 0) {
			break;
		}

		rc = zdb_kv_delete(kv, key);
		if (rc == ZDB_OK) {
			/*
			 * The entry was removed and the index compacted, so the
			 * next candidate has shifted into this position.
			 */
			continue;
		}

		if (rc == ZDB_ERR_NOT_FOUND) {
			/*
			 * The index outlived its record (a crash between the two
			 * writes). Drop the stale entry so the scan can advance.
			 */
			if (zdb_lock_write(kv->db) == ZDB_OK) {
				zdb_kv_ctx_track_delete(kv->db, kv->namespace_name, key);
				zdb_unlock_write(kv->db);
			}
			continue;
		}

		/* A real failure: remember it and step over the key. */
		if (first_error == ZDB_OK) {
			first_error = rc;
		}
		pos = (size_t)entry_pos + 1U;
	}

	/* Re-seed whatever the defaults table says this namespace should hold. */
	rc = zdb_kv_defaults_apply_ns(kv->db, kv->namespace_name);
	if ((rc != ZDB_OK) && (first_error == ZDB_OK)) {
		first_error = rc;
	}

	return first_error;
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
