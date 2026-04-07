/*
 * ZephyrDB public API (first pass)
 *
 * Scope: Core + KV + TS modules + Stage 2 FlatBuffers bootstrap helpers.
 */

#ifndef ZEPHYRDB_H_
#define ZEPHYRDB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Memory helpers
 */
#define ZDB_ALIGN_UP(value, align) ROUND_UP((value), (align))

/*
 * Generic slab declaration helper for static, fragmentation-free allocation.
 */
#define ZDB_MEM_SLAB_DEFINE(name, block_size, block_count, align)                              \
	BUILD_ASSERT((block_size) > 0, "ZephyrDB slab block size must be > 0");                     \
	BUILD_ASSERT((block_count) > 0, "ZephyrDB slab block count must be > 0");                   \
	K_MEM_SLAB_DEFINE_STATIC(name, ZDB_ALIGN_UP((block_size), (align)), (block_count), (align))

/*
 * First-pass convenience slab macros backed by Kconfig boundaries.
 */
#define ZDB_DEFINE_CORE_SLAB(name)                                                              \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_CORE_SLAB_BLOCK_SIZE,                                   \
			    CONFIG_ZDB_CORE_SLAB_BLOCK_COUNT, 4)

#define ZDB_DEFINE_CURSOR_SLAB(name)                                                            \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE,                                 \
			    CONFIG_ZDB_CURSOR_SLAB_BLOCK_COUNT, 4)

#define ZDB_DEFINE_KV_IO_SLAB(name)                                                             \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE,                                  \
			    CONFIG_ZDB_KV_IO_SLAB_BLOCK_COUNT, 4)

#define ZDB_DEFINE_TS_INGEST_SLAB(name)                                                         \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE,                              \
			    CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_COUNT, 4)

/*
 * Core types
 */
typedef enum {
	ZDB_OK = 0,
	ZDB_ERR_INVAL,
	ZDB_ERR_NOMEM,
	ZDB_ERR_NOT_FOUND,
	ZDB_ERR_IO,
	ZDB_ERR_BUSY,
	ZDB_ERR_TIMEOUT,
	ZDB_ERR_UNSUPPORTED,
	ZDB_ERR_CORRUPT,
	ZDB_ERR_INTERNAL,
} zdb_status_t;

typedef enum {
	ZDB_MODEL_CORE = 0,
	ZDB_MODEL_KV,
	ZDB_MODEL_TS,
} zdb_model_t;

typedef enum {
	ZDB_BACKEND_NONE = 0,
	ZDB_BACKEND_NVS,
	ZDB_BACKEND_LFS,
} zdb_backend_t;

typedef enum {
	ZDB_HEALTH_OK = 0,
	ZDB_HEALTH_DEGRADED,
	ZDB_HEALTH_READONLY,
	ZDB_HEALTH_FAULT,
} zdb_health_t;

typedef struct {
	const uint8_t *data;
	size_t len;
} zdb_bytes_t;

typedef struct {
	uint32_t ts_recover_runs;
	uint32_t ts_recover_failures;
	uint64_t ts_recover_truncated_bytes;
	uint32_t ts_crc_failures;
	uint32_t ts_corrupt_records;
	uint32_t ts_unsupported_versions;
} zdb_stats_t;

typedef struct {
	uint32_t recover_runs;
	uint32_t recover_failures;
	uint64_t recover_truncated_bytes;
	uint32_t crc_failures;
	uint32_t corrupt_records;
	uint32_t unsupported_versions;
} zdb_ts_stats_t;

/*
 * Compact telemetry export format for transport/logging.
 * Includes CRC for integrity during transmission.
 */
typedef struct __packed {
	uint16_t version;
	uint16_t reserved;
	uint32_t crc;
	uint32_t recover_runs;
	uint32_t recover_failures;
	uint64_t recover_truncated_bytes;
	uint32_t crc_failures;
	uint32_t corrupt_records;
	uint32_t unsupported_versions;
} zdb_ts_stats_export_t;

typedef struct {
	/*
	 * First-pass contract:
	 * - KV path expects partition_ref to point to an initialized struct nvs_fs.
	 * - TS path reserves this pointer for future storage object binding.
	 */
	const void *partition_ref;
	const char *lfs_mount_point;
	const char *kv_namespace;
	struct k_work_q *work_q;
	uint16_t scan_yield_every_n;
} zdb_cfg_t;

typedef struct {
	struct k_mutex rwlock;
	struct k_mem_slab *core_slab;
	struct k_mem_slab *cursor_slab;
	struct k_mem_slab *kv_io_slab;
	struct k_mem_slab *ts_ingest_slab;
	void *core_ctx;
	void *kv_ctx;
	void *ts_ctx;
	const zdb_cfg_t *cfg;
	zdb_stats_t stats;
} zdb_t;

typedef bool (*zdb_predicate_fn)(zdb_model_t model, const zdb_bytes_t *record, void *user_ctx);

/*
 * Cursor framework for cooperative scans.
 * Implementations should call k_yield according to scan_yield_every_n.
 */
typedef struct zdb_cursor {
	zdb_model_t model;
	zdb_backend_t backend;
	uint32_t flags;
	uint32_t iter_count;
	uint64_t position;
	zdb_bytes_t current;
	zdb_predicate_fn predicate;
	void *predicate_ctx;
	void *impl;
} zdb_cursor_t;

zdb_status_t zdb_init(zdb_t *db, const zdb_cfg_t *cfg);
zdb_status_t zdb_deinit(zdb_t *db);
zdb_health_t zdb_health(const zdb_t *db);
void zdb_stats_get(const zdb_t *db, zdb_stats_t *out_stats);
void zdb_stats_reset(zdb_t *db);
void zdb_ts_stats_get(const zdb_t *db, zdb_ts_stats_t *out_stats);
void zdb_ts_stats_reset(zdb_t *db);

/*
 * Export TS stats in compact, transport-ready format with CRC.
 */
zdb_status_t zdb_ts_stats_export(const zdb_t *db, zdb_ts_stats_export_t *out_export);

/*
 * Validate exported TS stats CRC on deserialization.
 * Returns ZDB_OK if valid, ZDB_ERR_CORRUPT if CRC mismatch.
 */
zdb_status_t zdb_ts_stats_export_validate(const zdb_ts_stats_export_t *export);

zdb_status_t zdb_cursor_reset(zdb_cursor_t *cursor);
zdb_status_t zdb_cursor_close(zdb_cursor_t *cursor);

/*
 * KV module (NVS-backed)
 */
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
typedef struct {
	zdb_t *db;
	const char *namespace_name;
} zdb_kv_t;

zdb_status_t zdb_kv_open(zdb_t *db, const char *namespace_name, zdb_kv_t *kv);
zdb_status_t zdb_kv_close(zdb_kv_t *kv);

zdb_status_t zdb_kv_set(zdb_kv_t *kv, const char *key, const void *value, size_t value_len);
zdb_status_t zdb_kv_get(zdb_kv_t *kv, const char *key, void *out_value,
			size_t out_capacity, size_t *out_len);
zdb_status_t zdb_kv_delete(zdb_kv_t *kv, const char *key);

/*
 * Exposes key->numeric-id mapping required by NVS.
 * Hash algorithm is implementation-defined but stable within a release line.
 */
uint16_t zdb_kv_key_to_id(const char *key);
#endif /* CONFIG_ZDB_KV */

/*
 * Time-series module (LittleFS append-log)
 */
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
typedef struct {
	zdb_t *db;
	const char *stream_name;
} zdb_ts_t;

typedef struct {
	uint64_t ts_ms;
	int64_t value;
} zdb_ts_sample_i64_t;

typedef enum {
	ZDB_TS_AGG_MIN = 0,
	ZDB_TS_AGG_MAX,
	ZDB_TS_AGG_AVG,
	ZDB_TS_AGG_SUM,
	ZDB_TS_AGG_COUNT,
} zdb_ts_agg_t;

typedef struct {
	uint64_t from_ts_ms;
	uint64_t to_ts_ms;
} zdb_ts_window_t;

typedef struct {
	zdb_ts_agg_t agg;
	double value;
	uint32_t points;
} zdb_ts_agg_result_t;

zdb_status_t zdb_ts_open(zdb_t *db, const char *stream_name, zdb_ts_t *ts);
zdb_status_t zdb_ts_close(zdb_ts_t *ts);

zdb_status_t zdb_ts_append_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *sample);
zdb_status_t zdb_ts_append_batch_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *samples,
			      size_t sample_count);

/*
 * Export a TS sample as a minimal FlatBuffer (root struct: { ts_ms, value }).
 * - If out_buf is NULL and out_capacity is 0, out_len receives required size.
 * - Requires CONFIG_ZDB_FLATBUFFERS=y and CONFIG_FLATCC=y.
 */
zdb_status_t zdb_ts_sample_i64_export_flatbuffer(const zdb_ts_sample_i64_t *sample,
						  uint8_t *out_buf,
						  size_t out_capacity,
						  size_t *out_len);

/*
 * Schedules background flush through Zephyr workqueue.
 */
zdb_status_t zdb_ts_flush_async(zdb_ts_t *ts);
zdb_status_t zdb_ts_flush_sync(zdb_ts_t *ts, k_timeout_t timeout);

zdb_status_t zdb_ts_query_aggregate(zdb_ts_t *ts, zdb_ts_window_t window,
			    zdb_ts_agg_t agg, zdb_ts_agg_result_t *out_result);

/*
 * Scans stream file and truncates trailing invalid/partial records.
 * out_truncated_bytes may be NULL.
 */
zdb_status_t zdb_ts_recover_stream(zdb_ts_t *ts, size_t *out_truncated_bytes);

zdb_status_t zdb_ts_cursor_open(zdb_ts_t *ts, zdb_ts_window_t window,
			zdb_predicate_fn predicate, void *predicate_ctx,
			zdb_cursor_t *out_cursor);
zdb_status_t zdb_cursor_next(zdb_cursor_t *cursor, zdb_bytes_t *out_record);
#endif /* CONFIG_ZDB_TS */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYRDB_H_ */
