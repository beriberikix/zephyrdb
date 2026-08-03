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
#define ZDB_TS_WMK_MAGIC 0x5A444257u
#define ZDB_TS_WMK_VERSION 1u
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

#if defined(CONFIG_ZDB_TS_ROLLOVER) && (CONFIG_ZDB_TS_ROLLOVER)
#define ZDB_TS_ROLLOVER_ENABLED 1
#else
#define ZDB_TS_ROLLOVER_ENABLED 0
#endif

#if defined(CONFIG_ZDB_TS_DELTA_ENCODING) && (CONFIG_ZDB_TS_DELTA_ENCODING)
#define ZDB_TS_DELTA_ENABLED 1
#else
#define ZDB_TS_DELTA_ENABLED 0
#endif

/*
 * Records are split across segment files when the oldest must be discardable
 * (rollover) or when each needs its own timestamp base (delta encoding).
 * Discarding is rollover's job alone.
 */
#if ZDB_TS_ROLLOVER_ENABLED || ZDB_TS_DELTA_ENABLED
#define ZDB_TS_SEGMENTED 1
#else
#define ZDB_TS_SEGMENTED 0
#endif

#ifndef CONFIG_ZDB_TS_ROLLOVER_SEGMENT_BYTES
#define CONFIG_ZDB_TS_ROLLOVER_SEGMENT_BYTES 4096
#endif
#ifndef CONFIG_ZDB_TS_ROLLOVER_MAX_SEGMENTS
#define CONFIG_ZDB_TS_ROLLOVER_MAX_SEGMENTS 4
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

#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
/*
 * Write missing defaults for one namespace, or every namespace in the table
 * when namespace_filter is NULL. Lives in the KV module so the core does not
 * need to know about backends; zdb_init() calls it through this hook.
 */
zdb_status_t zdb_kv_defaults_apply_ns(zdb_t *db, const char *namespace_filter);
#endif

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

/*
 * Compact record format.
 *
 * The per-record magic and version of the v1 layout repeat what the segment
 * header already states, and a full 64-bit absolute timestamp repeats most of
 * the previous record. Storing the timestamp as an offset from a base recorded
 * once per segment, and dropping the redundant fields, halves the record
 * almost exactly: 28 bytes to 16.
 *
 * The offset is from the segment's base rather than the previous record, so
 * every record still decodes on its own — which is what lets a cursor seek
 * backwards and lets COUNT work from file sizes.
 */
struct __packed zdb_ts_record_v2 {
	uint32_t ts_delta_ms_le;
	uint64_t value_le;
	uint32_t crc_le;
};

BUILD_ASSERT(sizeof(struct zdb_ts_record_v2) == 16,
	     "Unexpected TS compact record layout size");

/*
 * Segment header carrying the timestamp base its records offset from. Written
 * for segments holding compact records; the 16-byte header above still
 * introduces segments of v1 records.
 */
struct __packed zdb_ts_stream_header_v2 {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t reserved_le;
	uint32_t stream_id_le;
	uint64_t base_ts_ms_le;
	uint32_t crc_le;
};

BUILD_ASSERT(sizeof(struct zdb_ts_stream_header_v2) == 24,
	     "Unexpected TS compact stream header layout size");

/* Largest header any supported format uses, for staging buffers. */
#define ZDB_TS_HDR_MAX_SIZE sizeof(struct zdb_ts_stream_header_v2)

#define ZDB_TS_STREAM_VERSION_V2 2u

struct __packed zdb_ts_watermark_rec {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t reserved_le;
	uint64_t ts_ms_le;
	uint32_t crc_le;
};

BUILD_ASSERT(sizeof(struct zdb_ts_watermark_rec) == 20,
	     "Unexpected TS watermark layout size");

struct zdb_ts_cursor_ctx {
	zdb_t *db;
	const char *stream_name;
	zdb_ts_window_t window;
	struct zdb_ts_record_i64 cache;
	size_t file_offset;
	size_t ram_offset;
	bool file_done;
#if ZDB_TS_USE_LITTLEFS && ZDB_TS_SEGMENTED
	/*
	 * Segment being read, and the window snapshotted at open. Segments
	 * discarded while the cursor is open simply read as absent.
	 */
	uint32_t cur_seg;
	uint32_t seg_lo;
	uint32_t seg_hi;
#endif
#if ZDB_TS_USE_LITTLEFS
	/* Layout of the file being read, learned from its header. */
	size_t hdr_size;
	size_t rec_size;
	uint64_t base_ts_ms;
#endif
	/*
	 * Decoded form of the record most recently yielded. The cursor has to
	 * decode a record to apply the window and predicate, so consumers read
	 * the result here instead of decoding the raw bytes a second time —
	 * which also keeps them from having to know the record's format.
	 */
	uint64_t last_ts_ms;
	int64_t last_value;
	/*
	 * Direction is held here rather than in zdb_cursor_t::flags, which
	 * zdb_cursor_reset() clears.
	 */
	bool descending;
#if ZDB_TS_USE_LITTLEFS
	struct fs_file_t file;
	/* Size at open, so a descending walk knows where to start. */
	size_t file_size;
	bool file_open;
#endif
#if ZDB_TS_USE_FCB
	struct fcb_entry fcb_loc;
	bool fcb_started;
#endif
};

#ifndef CONFIG_ZDB_TS_MAX_STREAMS
#define CONFIG_ZDB_TS_MAX_STREAMS 1
#endif

/*
 * One open stream.
 *
 * The name is copied rather than referenced: a caller's zdb_ts_t may live on
 * the stack, while the slot outlives it until the stream is closed.
 */
struct zdb_ts_stream_ctx {
	char name[CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN + 1U];
	uint8_t *ingest_buf;
	size_t ingest_capacity;
	size_t ingest_used;
	/*
	 * Open handles referring to this stream. Re-opening a stream is
	 * allowed, so the slot is only released once every handle is closed —
	 * otherwise closing one would pull the buffer out from under another.
	 */
	uint8_t open_count;
	bool in_use;
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	/*
	 * Flushing and segment rollover both happen deep inside locked
	 * sections, but listeners must never run there. What happened is
	 * recorded here and published by the caller once it has unlocked.
	 */
	size_t pending_flush_bytes;
	size_t pending_discarded_bytes;
#endif
#if ZDB_TS_USE_LITTLEFS && ZDB_TS_SEGMENTED
	/*
	 * Segment window. Records live in <stream>.NNNN.zts files so the
	 * oldest can be discarded whole and each can carry its own timestamp
	 * base; these track which files exist and how full the newest one is.
	 */
	uint32_t oldest_seg;
	uint32_t cur_seg;
	size_t cur_seg_bytes;
	bool segs_scanned;
	/*
	 * Format of the segment currently being appended to. A stream keeps
	 * whichever format its segment was created with, so records within one
	 * file are always the same size.
	 */
	uint16_t seg_version;
	size_t seg_hdr_size;
	size_t seg_rec_size;
	uint64_t seg_base_ts_ms;
#endif
};

struct zdb_ts_core_ctx {
	struct k_work flush_work;
	struct k_sem flush_done;
	struct k_work_q *work_q;
	zdb_t *db;
	struct zdb_ts_stream_ctx streams[CONFIG_ZDB_TS_MAX_STREAMS];
	bool flush_pending;
	bool ts_dir_ready;
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	/*
	 * Closing a stream flushes it and then wipes the slot, so anything the
	 * slot still owed a listener is moved here first and published with the
	 * rest when the lock is released.
	 */
	size_t released_flush_bytes;
	size_t released_discarded_bytes;
	char released_stream[CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN + 1U];
#endif
#if ZDB_TS_USE_FCB
	struct fcb ts_fcb;
	struct flash_sector ts_fcb_sectors[CONFIG_ZDB_TS_FCB_SECTOR_COUNT];
	bool fcb_initialized;
#endif
};

/*
 * These contexts are carved out of fixed-size slab blocks, so a context that
 * outgrows its block silently writes into the next one and corrupts the slab's
 * free list. Both structures depend on pointer width and on the selected
 * backend, so the sizes differ between targets; assert at build time rather
 * than discovering it as a kernel assertion under load.
 */
#if defined(CONFIG_ZDB_CORE_SLAB_BLOCK_SIZE)
BUILD_ASSERT(sizeof(struct zdb_ts_core_ctx) <= CONFIG_ZDB_CORE_SLAB_BLOCK_SIZE,
	     "CONFIG_ZDB_CORE_SLAB_BLOCK_SIZE is too small for struct zdb_ts_core_ctx");
#endif
#if defined(CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE)
BUILD_ASSERT(sizeof(struct zdb_ts_cursor_ctx) <= CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE,
	     "CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE is too small for struct zdb_ts_cursor_ctx");
#endif

#if ZDB_TS_USE_LITTLEFS && defined(CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_COUNT)
/* Each open stream holds one ingest block for as long as it is open. */
BUILD_ASSERT(CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_COUNT >= CONFIG_ZDB_TS_MAX_STREAMS,
	     "CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_COUNT must cover CONFIG_ZDB_TS_MAX_STREAMS");
#endif

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
void zdb_emit_core_event(zdb_t *db, zdb_core_event_type_t type, zdb_health_t prev_health,
			 zdb_health_t health, zdb_status_t status);
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
/*
 * Collect flush and rollover activity recorded while the instance lock was
 * held, clearing it so it is reported once. Call with the lock held; the
 * caller publishes after releasing it (see zdb_unlock_read()/zdb_unlock_write()).
 */
void zdb_ts_take_deferred_events(zdb_t *db, size_t *out_flushed, size_t *out_discarded,
				 char *out_stream, size_t out_stream_size);
#endif
#endif /* CONFIG_ZDB_EVENTING */

#endif /* ZEPHYRDB_INTERNAL_H_ */
