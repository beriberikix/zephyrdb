/* Shared internal declarations for ZephyrDB modules */
#ifndef ZEPHYRDB_INTERNAL_H_
#define ZEPHYRDB_INTERNAL_H_

#include "zephyrdb.h"

#include <sys/types.h>

/*
 * Safe defaults for unit tests compiled outside full Kconfig integration.
 */
#ifndef CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN
#define CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN 24
#endif
#ifndef CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE
#define CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE 64
#endif
#ifndef CONFIG_ZDB_TS_INGEST_BUFFER_BYTES
#define CONFIG_ZDB_TS_INGEST_BUFFER_BYTES 1024
#endif
#ifndef CONFIG_ZDB_TS_MAX_AGG_POINTS
#define CONFIG_ZDB_TS_MAX_AGG_POINTS 4096
#endif
#ifndef CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES
#define CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES 4096
#endif
#ifndef CONFIG_ZDB_TS_DIRNAME
#define CONFIG_ZDB_TS_DIRNAME "zdb"
#endif

#define ZDB_TS_PATH_MAX 128
#define ZDB_TS_REC_MAGIC 0x5A445442u
#define ZDB_TS_REC_VERSION 1u
#define ZDB_TS_STREAM_MAGIC 0x5A445453u
#define ZDB_TS_STREAM_VERSION 1u
#define ZDB_TS_FCB_MAGIC 0x5A444246u
#define ZDB_TS_FCB_VERSION 1u

#if defined(CONFIG_ZDB_TS_BACKEND_LITTLEFS) && (CONFIG_ZDB_TS_BACKEND_LITTLEFS)
#define ZDB_TS_USE_LITTLEFS 1
#else
#define ZDB_TS_USE_LITTLEFS 0
#endif

#if defined(CONFIG_ZDB_TS_BACKEND_FCB) && (CONFIG_ZDB_TS_BACKEND_FCB)
#define ZDB_TS_USE_FCB 1
#else
#define ZDB_TS_USE_FCB 0
#endif

#if defined(CONFIG_ZDB_STATS) && (CONFIG_ZDB_STATS)
#define ZDB_STATS_ENABLED 1
#else
#define ZDB_STATS_ENABLED 0
#endif

#define ZDB_STAT_INC(db, field)                                                                \
	do {                                                                                         \
		if (ZDB_STATS_ENABLED && ((db) != NULL)) {                                                \
			(db)->ts_stats.field++;                                                                  \
		}                                                                                        \
	} while (0)

#define ZDB_STAT_ADD(db, field, value)                                                         \
	do {                                                                                         \
		if (ZDB_STATS_ENABLED && ((db) != NULL)) {                                                \
			(db)->ts_stats.field += (value);                                                         \
		}                                                                                        \
	} while (0)

/* Core utility functions (non-static, shared across modules) */
zdb_status_t zdb_status_from_errno(int err);
zdb_status_t zdb_lock_read(zdb_t *db);
zdb_status_t zdb_lock_write(zdb_t *db);
void zdb_unlock_read(zdb_t *db);
void zdb_unlock_write(zdb_t *db);
zdb_status_t zdb_cursor_reset(zdb_cursor_t *cursor);

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)

#if ZDB_TS_USE_LITTLEFS
#include <zephyr/fs/fs.h>
#endif
#if ZDB_TS_USE_FCB
#include <zephyr/fs/fcb.h>
#endif

void zdb_health_check(zdb_t *db);

struct __packed zdb_ts_record_i64 {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t reserved_le;
	uint64_t ts_ms_le;
	uint64_t value_le;
	uint32_t crc_le;
};

BUILD_ASSERT(sizeof(struct zdb_ts_record_i64) == 28,
	     "Unexpected TS record layout size");

struct __packed zdb_ts_stream_header {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t reserved_le;
	uint32_t stream_id_le;
	uint32_t crc_le;
};

BUILD_ASSERT(sizeof(struct zdb_ts_stream_header) == 16,
	     "Unexpected TS stream header layout size");

struct zdb_ts_cursor_ctx {
	zdb_t *db;
	const char *stream_name;
	zdb_ts_window_t window;
	struct zdb_ts_record_i64 cache;
	size_t file_offset;
	size_t ram_offset;
	bool file_done;
#if ZDB_TS_USE_LITTLEFS
	struct fs_file_t file;
	bool file_open;
#endif
#if ZDB_TS_USE_FCB
	struct fcb_entry fcb_loc;
	bool fcb_started;
#endif
};

struct zdb_ts_core_ctx {
	struct k_work flush_work;
	struct k_sem flush_done;
	struct k_work_q *work_q;
	zdb_t *db;
	uint8_t *ingest_buf;
	size_t ingest_capacity;
	size_t ingest_used;
	const char *active_stream;
	bool flush_pending;
	bool ts_dir_ready;
#if ZDB_TS_USE_FCB
	struct fcb ts_fcb;
	struct flash_sector ts_fcb_sectors[CONFIG_ZDB_TS_FCB_SECTOR_COUNT];
	bool fcb_initialized;
#endif
};

#endif /* CONFIG_ZDB_TS */

/* Event emission functions (non-static, shared across modules) */
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
void zdb_emit_kv_event(zdb_t *db, zdb_event_type_t type, const char *namespace_name,
		       const char *key, size_t value_len, zdb_status_t status);
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
void zdb_emit_ts_event(zdb_t *db, zdb_ts_event_type_t type, const char *stream_name,
		       uint64_t sample_ts_ms, int64_t sample_value,
		       size_t flushed_bytes, size_t truncated_bytes,
		       zdb_status_t status);
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
void zdb_emit_doc_event(zdb_t *db, zdb_doc_event_type_t type,
			const char *collection_name, const char *document_id,
			size_t field_count, size_t serialized_bytes,
			zdb_status_t status);
#endif
#endif /* CONFIG_ZDB_EVENTING */

#endif /* ZEPHYRDB_INTERNAL_H_ */
