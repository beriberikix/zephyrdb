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

static bool zdb_key_valid(const char *key)
{
	size_t key_len;

	if ((key == NULL) || ((*key) == '\0')) {
		return false;
	}

	key_len = strlen(key);
	return (key_len <= (size_t)CONFIG_ZDB_MAX_KEY_LEN);
}

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

static uint32_t zdb_kv_key_to_id(const char *key);

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

	if ((kv == NULL) || (kv->db == NULL) || (value == NULL) || (value_len == 0U) ||
	    !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	id = zdb_kv_key_to_id(key);
	lock_rc = zdb_lock_write(kv->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	wr = zdb_kv_backend_write(kv->db, id, value, value_len);
	zdb_unlock_write(kv->db);

	if (wr < 0) {
		status = zdb_status_from_errno((int)wr);
	} else if ((size_t)wr != value_len) {
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

	if ((kv == NULL) || (kv->db == NULL) || (out_len == NULL) || !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if ((out_value == NULL) && (out_capacity > 0U)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	id = zdb_kv_key_to_id(key);
	lock_rc = zdb_lock_read(kv->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rd = zdb_kv_backend_read(kv->db, id, out_value, out_capacity);
	zdb_unlock_read(kv->db);

	if (rd < 0) {
		*out_len = 0U;
		return zdb_status_from_errno((int)rd);
	}

	*out_len = (size_t)rd;
	return ZDB_OK;
}

zdb_status_t zdb_kv_delete(zdb_kv_t *kv, const char *key)
{
	uint32_t id;
	int rc;
	zdb_status_t lock_rc;
	zdb_status_t status;

	if ((kv == NULL) || (kv->db == NULL) || !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	id = zdb_kv_key_to_id(key);
	lock_rc = zdb_lock_write(kv->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rc = zdb_kv_backend_delete(kv->db, id);
	zdb_unlock_write(kv->db);

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

#endif /* CONFIG_ZDB_KV */
