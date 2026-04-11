/* Core infrastructure implementation */
#include "zephyrdb_internal.h"

#include <errno.h>
#include <string.h>
#include <zephyr/sys/crc.h>
#include <zephyr/kernel.h>

#if ZDB_TS_USE_LITTLEFS
#include <zephyr/fs/fs.h>
#endif

#if defined(CONFIG_ZDB_EVENTING_ZBUS) && (CONFIG_ZDB_EVENTING_ZBUS)
#include "zephyrdb_eventing_zbus.h"
#endif

zdb_status_t zdb_status_from_errno(int err)
{
	int e = (err < 0) ? -err : err;

	switch (e) {
	case 0:
		return ZDB_OK;
	case EINVAL:
		return ZDB_ERR_INVAL;
	case ENOMEM:
		return ZDB_ERR_NOMEM;
	case ENOENT:
		return ZDB_ERR_NOT_FOUND;
	case EBUSY:
		return ZDB_ERR_BUSY;
	case ETIMEDOUT:
		return ZDB_ERR_TIMEOUT;
	case ENOTSUP:
		return ZDB_ERR_UNSUPPORTED;
	default:
		return ZDB_ERR_IO;
	}
}

const char *zdb_status_str(zdb_status_t status)
{
	switch (status) {
	case ZDB_OK:
		return "OK";
	case ZDB_ERR_INVAL:
		return "EINVAL";
	case ZDB_ERR_NOMEM:
		return "ENOMEM";
	case ZDB_ERR_NOT_FOUND:
		return "NOT_FOUND";
	case ZDB_ERR_IO:
		return "IO";
	case ZDB_ERR_BUSY:
		return "BUSY";
	case ZDB_ERR_TIMEOUT:
		return "TIMEOUT";
	case ZDB_ERR_UNSUPPORTED:
		return "UNSUPPORTED";
	case ZDB_ERR_CORRUPT:
		return "CORRUPT";
	case ZDB_ERR_INTERNAL:
		return "INTERNAL";
	default:
		return "UNKNOWN";
	}
}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
void zdb_health_check(zdb_t *db)
{
	if (db == NULL) {
		return;
	}

	if ((db->ts_stats.corrupt_records > 0U) ||
	    (db->ts_stats.crc_failures > 0U) ||
	    (db->ts_stats.recover_failures > 0U)) {
		if (db->health < ZDB_HEALTH_DEGRADED) {
			db->health = ZDB_HEALTH_DEGRADED;
		}
	}
}
#endif /* CONFIG_ZDB_TS */

zdb_status_t zdb_lock_read(zdb_t *db)
{
	int rc = k_rwlock_read_lock(&db->rwlock, K_FOREVER);
	return (rc == 0) ? ZDB_OK : ZDB_ERR_BUSY;
}

zdb_status_t zdb_lock_write(zdb_t *db)
{
	int rc = k_rwlock_write_lock(&db->rwlock, K_FOREVER);
	return (rc == 0) ? ZDB_OK : ZDB_ERR_BUSY;
}

void zdb_unlock_read(zdb_t *db)
{
	k_rwlock_read_unlock(&db->rwlock);
}

void zdb_unlock_write(zdb_t *db)
{
	k_rwlock_write_unlock(&db->rwlock);
}

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
void zdb_emit_kv_event(zdb_t *db, zdb_event_type_t type, const char *namespace_name,
		       const char *key, size_t value_len, zdb_status_t status)
{
	zdb_kv_event_t event;
	size_t i;

	if (db == NULL) {
		return;
	}

	event.type = type;
	strncpy(event.namespace_name, namespace_name,
		sizeof(event.namespace_name) - 1);
	event.namespace_name[sizeof(event.namespace_name) - 1] = '\0';
	strncpy(event.key, key, sizeof(event.key) - 1);
	event.key[sizeof(event.key) - 1] = '\0';
	event.value_len = value_len;
	event.timestamp_ms = (uint64_t)k_uptime_get();
	event.status = status;

#if defined(CONFIG_ZDB_EVENTING_ZBUS) && (CONFIG_ZDB_EVENTING_ZBUS)
	/* zbus publishing is best-effort and must not affect DB operation status. */
	(void)zdb_eventing_zbus_publish(&event);
#endif

	if ((db->event_listeners != NULL) && (db->event_listener_count > 0U)) {
		for (i = 0U; i < db->event_listener_count; i++) {
			const zdb_event_listener_t *listener = &db->event_listeners[i];

			if (listener->notify != NULL) {
				listener->notify(&event, listener->user_ctx);
			}
		}
	}
}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
void zdb_emit_ts_event(zdb_t *db, zdb_ts_event_type_t type, const char *stream_name,
		       uint64_t sample_ts_ms, int64_t sample_value,
		       size_t flushed_bytes, size_t truncated_bytes,
		       zdb_status_t status)
{
	zdb_ts_event_t event;
	size_t i;

	if (db == NULL) {
		return;
	}

	event.type = type;
	strncpy(event.stream_name, stream_name,
		sizeof(event.stream_name) - 1);
	event.stream_name[sizeof(event.stream_name) - 1] = '\0';
	event.timestamp_ms = (uint64_t)k_uptime_get();
	event.sample_ts_ms = sample_ts_ms;
	event.sample_value = sample_value;
	event.flushed_bytes = flushed_bytes;
	event.truncated_bytes = truncated_bytes;
	event.status = status;

#if defined(CONFIG_ZDB_EVENTING_ZBUS) && (CONFIG_ZDB_EVENTING_ZBUS)
	(void)zdb_eventing_zbus_publish_ts(&event);
#endif

	if ((db->ts_event_listeners != NULL) && (db->ts_event_listener_count > 0U)) {
		for (i = 0U; i < db->ts_event_listener_count; i++) {
			const zdb_ts_event_listener_t *listener = &db->ts_event_listeners[i];

			if (listener->notify != NULL) {
				listener->notify(&event, listener->user_ctx);
			}
		}
	}
}
#endif /* CONFIG_ZDB_TS */

#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
void zdb_emit_doc_event(zdb_t *db, zdb_doc_event_type_t type,
			const char *collection_name, const char *document_id,
			size_t field_count, size_t serialized_bytes,
			zdb_status_t status)
{
	zdb_doc_event_t event;
	size_t i;

	if (db == NULL) {
		return;
	}

	event.type = type;
	strncpy(event.collection_name, collection_name,
		sizeof(event.collection_name) - 1);
	event.collection_name[sizeof(event.collection_name) - 1] = '\0';
	strncpy(event.document_id, document_id,
		sizeof(event.document_id) - 1);
	event.document_id[sizeof(event.document_id) - 1] = '\0';
	event.timestamp_ms = (uint64_t)k_uptime_get();
	event.field_count = field_count;
	event.serialized_bytes = serialized_bytes;
	event.status = status;

#if defined(CONFIG_ZDB_EVENTING_ZBUS) && (CONFIG_ZDB_EVENTING_ZBUS)
	(void)zdb_eventing_zbus_publish_doc(&event);
#endif

	if ((db->doc_event_listeners != NULL) && (db->doc_event_listener_count > 0U)) {
		for (i = 0U; i < db->doc_event_listener_count; i++) {
			const zdb_doc_event_listener_t *listener = &db->doc_event_listeners[i];

			if (listener->notify != NULL) {
				listener->notify(&event, listener->user_ctx);
			}
		}
	}
}
#endif /* CONFIG_ZDB_DOC */
#endif

zdb_status_t zdb_init(zdb_t *db, const zdb_cfg_t *cfg)
{
	if ((db == NULL) || (cfg == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->core_slab == NULL) || (db->cursor_slab == NULL)) {
		return ZDB_ERR_INVAL;
	}

	k_rwlock_init(&db->rwlock);
	db->cfg = cfg;
	db->core_ctx = NULL;
	db->kv_ctx = NULL;
	db->ts_ctx = NULL;
	(void)memset(&db->ts_stats, 0, sizeof(db->ts_stats));
	db->health = ZDB_HEALTH_OK;
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	db->event_listeners = cfg->event_listeners;
	db->event_listener_count = cfg->event_listener_count;
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	db->ts_event_listeners = cfg->ts_event_listeners;
	db->ts_event_listener_count = cfg->ts_event_listener_count;
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
	db->doc_event_listeners = cfg->doc_event_listeners;
	db->doc_event_listener_count = cfg->doc_event_listener_count;
#endif
#endif

	return ZDB_OK;
}

zdb_status_t zdb_deinit(zdb_t *db)
{
	if (db == NULL) {
		return ZDB_ERR_INVAL;
	}

	if (db->kv_ctx != NULL) {
		k_free(db->kv_ctx);
	}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	if ((db->core_slab != NULL) && (db->ts_ctx != NULL)) {
		struct zdb_ts_core_ctx *ctx = (struct zdb_ts_core_ctx *)db->ts_ctx;

		if ((db->ts_ingest_slab != NULL) && (ctx->ingest_buf != NULL)) {
			k_mem_slab_free(db->ts_ingest_slab, ctx->ingest_buf);
		}
		k_mem_slab_free(db->core_slab, ctx);
	}
#endif

	db->cfg = NULL;
	db->core_ctx = NULL;
	db->kv_ctx = NULL;
	db->ts_ctx = NULL;
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	db->event_listeners = NULL;
	db->event_listener_count = 0U;
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	db->ts_event_listeners = NULL;
	db->ts_event_listener_count = 0U;
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
	db->doc_event_listeners = NULL;
	db->doc_event_listener_count = 0U;
#endif
#endif

	return ZDB_OK;
}

zdb_health_t zdb_health(const zdb_t *db)
{
	if ((db == NULL) || (db->cfg == NULL)) {
		return ZDB_HEALTH_FAULT;
	}

	return db->health;
}

void zdb_ts_stats_get(const zdb_t *db, zdb_ts_stats_t *out_stats)
{
	if ((db == NULL) || (out_stats == NULL)) {
		return;
	}

	*out_stats = db->ts_stats;
}

void zdb_ts_stats_reset(zdb_t *db)
{
	if (db == NULL) {
		return;
	}

	(void)memset(&db->ts_stats, 0, sizeof(db->ts_stats));
}

zdb_status_t zdb_ts_stats_export(const zdb_t *db, zdb_ts_stats_export_t *out_export)
{
	uint32_t crc;

	if ((db == NULL) || (out_export == NULL)) {
		return ZDB_ERR_INVAL;
	}

	out_export->version = 1U;
	out_export->reserved = 0U;
	out_export->recover_runs = db->ts_stats.recover_runs;
	out_export->recover_failures = db->ts_stats.recover_failures;
	out_export->recover_truncated_bytes = db->ts_stats.recover_truncated_bytes;
	out_export->crc_failures = db->ts_stats.crc_failures;
	out_export->corrupt_records = db->ts_stats.corrupt_records;
	out_export->unsupported_versions = db->ts_stats.unsupported_versions;

	/* Compute CRC over all fields except the crc field itself */
	crc = crc32_ieee((const uint8_t *)out_export,
			 offsetof(zdb_ts_stats_export_t, crc));
	out_export->crc = crc;

	return ZDB_OK;
}

zdb_status_t zdb_ts_stats_export_validate(const zdb_ts_stats_export_t *export)
{
	uint32_t expect_crc;
	uint32_t got_crc;

	if (export == NULL) {
		return ZDB_ERR_INVAL;
	}

	/* Recompute CRC over all fields except the crc field itself */
	expect_crc = crc32_ieee((const uint8_t *)export,
			       offsetof(zdb_ts_stats_export_t, crc));
	got_crc = export->crc;

	if (got_crc != expect_crc) {
		return ZDB_ERR_CORRUPT;
	}

	/* Check version for forward compatibility */
	if (export->version != 1U) {
		return ZDB_ERR_UNSUPPORTED;
	}

	return ZDB_OK;
}

zdb_status_t zdb_cursor_reset(zdb_cursor_t *cursor)
{
	if (cursor == NULL) {
		return ZDB_ERR_INVAL;
	}

	cursor->flags = 0U;
	cursor->iter_count = 0U;
	cursor->position = 0U;
	cursor->current.data = NULL;
	cursor->current.len = 0U;

	return ZDB_OK;
}

zdb_status_t zdb_cursor_close(zdb_cursor_t *cursor)
{
	if (cursor == NULL) {
		return ZDB_ERR_INVAL;
	}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	if ((cursor->model == ZDB_MODEL_TS) && (cursor->impl != NULL)) {
		struct zdb_ts_cursor_ctx *ctx = (struct zdb_ts_cursor_ctx *)cursor->impl;

#if ZDB_TS_USE_LITTLEFS
		if (ctx->file_open) {
			(void)fs_close(&ctx->file);
			ctx->file_open = false;
		}
#endif
		if ((ctx->db != NULL) && (ctx->db->cursor_slab != NULL)) {
			k_mem_slab_free(ctx->db->cursor_slab, ctx);
		}
		cursor->impl = NULL;
	}
#endif

	return zdb_cursor_reset(cursor);
}
