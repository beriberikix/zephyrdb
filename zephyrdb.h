/*
 * ZephyrDB public API
 *
 * Core + KV + TS + DOC + Eventing + Shell modules.
 */

#ifndef ZEPHYRDB_H_
#define ZEPHYRDB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Memory helpers
 */
#define ZDB_ALIGN_UP(value, align) ROUND_UP((value), (align))

/*
 * Generic slab declaration helper for static, fragmentation-free allocation.
 */
#define ZDB_MEM_SLAB_DEFINE(name, block_size, block_count, align)           \
	BUILD_ASSERT((block_size) > 0, "ZephyrDB slab block size must be > 0");   \
	BUILD_ASSERT((block_count) > 0, "ZephyrDB slab block count must be > 0"); \
	K_MEM_SLAB_DEFINE_STATIC(name, ZDB_ALIGN_UP((block_size), (align)), (block_count), (align))

/*
 * First-pass convenience slab macros backed by Kconfig boundaries.
 */
#define ZDB_DEFINE_CORE_SLAB(name)                           \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_CORE_SLAB_BLOCK_SIZE, \
											CONFIG_ZDB_CORE_SLAB_BLOCK_COUNT, 4)

#define ZDB_DEFINE_CURSOR_SLAB(name)                           \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE, \
											CONFIG_ZDB_CURSOR_SLAB_BLOCK_COUNT, 4)

#define ZDB_DEFINE_KV_IO_SLAB(name)                           \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE, \
											CONFIG_ZDB_KV_IO_SLAB_BLOCK_COUNT, 4)

#define ZDB_DEFINE_TS_INGEST_SLAB(name)                           \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE, \
											CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_COUNT, 4)

	/*
	 * Core types
	 */
	typedef enum
	{
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

	typedef enum
	{
		ZDB_MODEL_CORE = 0,
		ZDB_MODEL_KV,
		ZDB_MODEL_TS,
	} zdb_model_t;

	typedef enum
	{
		ZDB_BACKEND_NONE = 0,
		ZDB_BACKEND_NVS,
		ZDB_BACKEND_LFS,
	} zdb_backend_t;

	typedef enum
	{
		ZDB_HEALTH_OK = 0,
		ZDB_HEALTH_DEGRADED,
		ZDB_HEALTH_READONLY,
		ZDB_HEALTH_FAULT,
	} zdb_health_t;

	typedef struct
	{
		const uint8_t *data;
		size_t len;
	} zdb_bytes_t;

	typedef struct
	{
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
	typedef struct __packed
	{
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

/*
 * Safe defaults for Kconfig symbols used in struct definitions.
 * These allow compilation in unit tests that bypass the Zephyr module system.
 */
#ifndef CONFIG_ZDB_MAX_KEY_LEN
#define CONFIG_ZDB_MAX_KEY_LEN 48
#endif

/*
 * Eventing types
 */
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)

	typedef enum
	{
		ZDB_EVENT_KV_SET = 0,
		ZDB_EVENT_KV_DELETE,
	} zdb_event_type_t;

	typedef struct
	{
		zdb_event_type_t type;
		char namespace_name[CONFIG_ZDB_MAX_KEY_LEN + 1];
		char key[CONFIG_ZDB_MAX_KEY_LEN + 1];
		size_t value_len;
		uint64_t timestamp_ms;
		zdb_status_t status;
	} zdb_kv_event_t;

	typedef struct
	{
		void (*notify)(const zdb_kv_event_t *event, void *user_ctx);
		void *user_ctx;
	} zdb_event_listener_t;

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)

#ifndef CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN
#define CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN 24
#endif

	typedef enum
	{
		ZDB_TS_EVENT_APPEND = 0,
		ZDB_TS_EVENT_FLUSH,
		ZDB_TS_EVENT_RECOVER,
	} zdb_ts_event_type_t;

	typedef struct
	{
		zdb_ts_event_type_t type;
		char stream_name[CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN + 1];
		uint64_t timestamp_ms;
		uint64_t sample_ts_ms;
		int64_t sample_value;
		size_t flushed_bytes;
		size_t truncated_bytes;
		zdb_status_t status;
	} zdb_ts_event_t;

	typedef struct
	{
		void (*notify)(const zdb_ts_event_t *event, void *user_ctx);
		void *user_ctx;
	} zdb_ts_event_listener_t;

#endif /* CONFIG_ZDB_TS */

#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)

	typedef enum
	{
		ZDB_DOC_EVENT_CREATE = 0,
		ZDB_DOC_EVENT_SAVE,
		ZDB_DOC_EVENT_DELETE,
	} zdb_doc_event_type_t;

	typedef struct
	{
		zdb_doc_event_type_t type;
		char collection_name[CONFIG_ZDB_MAX_KEY_LEN + 1];
		char document_id[CONFIG_ZDB_MAX_KEY_LEN + 1];
		uint64_t timestamp_ms;
		size_t field_count;
		size_t serialized_bytes;
		zdb_status_t status;
	} zdb_doc_event_t;

	typedef struct
	{
		void (*notify)(const zdb_doc_event_t *event, void *user_ctx);
		void *user_ctx;
	} zdb_doc_event_listener_t;

#endif /* CONFIG_ZDB_DOC */

#endif /* CONFIG_ZDB_EVENTING */

	typedef struct
	{
		const void *kv_backend_fs;
		const char *lfs_mount_point;
		struct k_work_q *work_q;
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
		const zdb_event_listener_t *event_listeners;
		size_t event_listener_count;
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
		const zdb_ts_event_listener_t *ts_event_listeners;
		size_t ts_event_listener_count;
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
		const zdb_doc_event_listener_t *doc_event_listeners;
		size_t doc_event_listener_count;
#endif
#endif
	} zdb_cfg_t;

	typedef struct
	{
		struct k_mutex lock;
		struct k_mem_slab *core_slab;
		struct k_mem_slab *cursor_slab;
		struct k_mem_slab *kv_io_slab;
		struct k_mem_slab *ts_ingest_slab;
		void *core_ctx;
		void *kv_ctx;
		void *ts_ctx;
		const zdb_cfg_t *cfg;
		zdb_ts_stats_t ts_stats;
		zdb_health_t health;
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
		const zdb_event_listener_t *event_listeners;
		size_t event_listener_count;
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
		const zdb_ts_event_listener_t *ts_event_listeners;
		size_t ts_event_listener_count;
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
		const zdb_doc_event_listener_t *doc_event_listeners;
		size_t doc_event_listener_count;
#endif
#endif
	} zdb_t;

/*
 * Static instance helper: declares Kconfig-backed slabs and wires them
 * into a zdb_t.  Call zdb_init(&name, &cfg) before first use.
 */
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
#define _ZDB_DEFINE_KV_IO_SLAB(name)  ZDB_DEFINE_KV_IO_SLAB(name);
#define _ZDB_KV_IO_SLAB_INIT(name)    .kv_io_slab = &name,
#else
#define _ZDB_DEFINE_KV_IO_SLAB(name)
#define _ZDB_KV_IO_SLAB_INIT(name)
#endif

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
#define _ZDB_DEFINE_TS_INGEST_SLAB(name)  ZDB_DEFINE_TS_INGEST_SLAB(name);
#define _ZDB_TS_INGEST_SLAB_INIT(name)    .ts_ingest_slab = &name,
#else
#define _ZDB_DEFINE_TS_INGEST_SLAB(name)
#define _ZDB_TS_INGEST_SLAB_INIT(name)
#endif

#define ZDB_DEFINE_STATIC(db_name, cfg_name)                                       \
	ZDB_DEFINE_CORE_SLAB(_zdb_core_##db_name);                                      \
	ZDB_DEFINE_CURSOR_SLAB(_zdb_cursor_##db_name);                                   \
	_ZDB_DEFINE_KV_IO_SLAB(_zdb_kv_io_##db_name)                                    \
	_ZDB_DEFINE_TS_INGEST_SLAB(_zdb_ts_ingest_##db_name)                             \
	static zdb_t db_name = {                                                          \
		.core_slab = &_zdb_core_##db_name,                                              \
		.cursor_slab = &_zdb_cursor_##db_name,                                          \
		_ZDB_KV_IO_SLAB_INIT(_zdb_kv_io_##db_name)                                     \
		_ZDB_TS_INGEST_SLAB_INIT(_zdb_ts_ingest_##db_name)                              \
	}

	typedef bool (*zdb_predicate_fn)(zdb_model_t model, const zdb_bytes_t *record, void *user_ctx);

	/*
	 * Cursor framework for cooperative scans.
	 */
	typedef struct zdb_cursor
	{
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

	/*
	 * Core API
	 */
	zdb_status_t zdb_init(zdb_t *db, const zdb_cfg_t *cfg);
	zdb_status_t zdb_deinit(zdb_t *db);
	zdb_health_t zdb_health(const zdb_t *db);
	const char *zdb_status_str(zdb_status_t status);

	void zdb_ts_stats_get(const zdb_t *db, zdb_ts_stats_t *out_stats);
	void zdb_ts_stats_reset(zdb_t *db);
	zdb_status_t zdb_ts_stats_export(const zdb_t *db, zdb_ts_stats_export_t *out_export);
	zdb_status_t zdb_ts_stats_export_validate(const zdb_ts_stats_export_t *export_data);

	zdb_status_t zdb_cursor_reset(zdb_cursor_t *cursor);
	zdb_status_t zdb_cursor_close(zdb_cursor_t *cursor);

/*
 * KV module
 */
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
	typedef struct
	{
		zdb_t *db;
		const char *namespace_name;
	} zdb_kv_t;

	zdb_status_t zdb_kv_open(zdb_t *db, const char *namespace_name, zdb_kv_t *kv);
	zdb_status_t zdb_kv_close(zdb_kv_t *kv);

	zdb_status_t zdb_kv_set(zdb_kv_t *kv, const char *key, const void *value, size_t value_len);
	zdb_status_t zdb_kv_get(zdb_kv_t *kv, const char *key, void *out_value,
							size_t out_capacity, size_t *out_len);
	zdb_status_t zdb_kv_delete(zdb_kv_t *kv, const char *key);
#endif /* CONFIG_ZDB_KV */

/*
 * Time-series module
 */
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	typedef struct
	{
		zdb_t *db;
		const char *stream_name;
	} zdb_ts_t;

	typedef struct
	{
		uint64_t ts_ms;
		int64_t value;
	} zdb_ts_sample_i64_t;

	typedef enum
	{
		ZDB_TS_AGG_MIN = 0,
		ZDB_TS_AGG_MAX,
		ZDB_TS_AGG_AVG,
		ZDB_TS_AGG_SUM,
		ZDB_TS_AGG_COUNT,
	} zdb_ts_agg_t;

	typedef struct
	{
		uint64_t from_ts_ms;
		uint64_t to_ts_ms;
	} zdb_ts_window_t;

#define ZDB_TS_WINDOW_ALL ((zdb_ts_window_t){.from_ts_ms = 0, .to_ts_ms = UINT64_MAX})

	typedef struct
	{
		zdb_ts_agg_t agg;
		double value;
		uint32_t points;
	} zdb_ts_agg_result_t;

	zdb_status_t zdb_ts_open(zdb_t *db, const char *stream_name, zdb_ts_t *ts);
	zdb_status_t zdb_ts_close(zdb_ts_t *ts);

	zdb_status_t zdb_ts_append_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *sample);
	zdb_status_t zdb_ts_append_batch_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *samples,
										 size_t sample_count);

	zdb_status_t zdb_ts_flush_async(zdb_ts_t *ts);
	zdb_status_t zdb_ts_flush_sync(zdb_ts_t *ts, k_timeout_t timeout);

	zdb_status_t zdb_ts_query_aggregate(zdb_ts_t *ts, zdb_ts_window_t window,
										zdb_ts_agg_t agg, zdb_ts_agg_result_t *out_result);

	zdb_status_t zdb_ts_recover_stream(zdb_ts_t *ts, size_t *out_truncated_bytes);

	zdb_status_t zdb_ts_cursor_open(zdb_ts_t *ts, zdb_ts_window_t window,
									zdb_predicate_fn predicate, void *predicate_ctx,
									zdb_cursor_t *out_cursor);
	zdb_status_t zdb_cursor_next(zdb_cursor_t *cursor, zdb_bytes_t *out_record);
#endif /* CONFIG_ZDB_TS */

/*
 * FlatBuffers helpers
 */
#if defined(CONFIG_ZDB_FLATBUFFERS) && (CONFIG_ZDB_FLATBUFFERS)
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	zdb_status_t zdb_ts_sample_i64_export_flatbuffer(const zdb_ts_sample_i64_t *sample,
													 uint8_t *out_buf, size_t out_capacity,
													 size_t *out_len);
#endif
#endif /* CONFIG_ZDB_FLATBUFFERS */

/*
 * Document model (Stage 3)
 */
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)

	typedef enum
	{
		ZDB_DOC_FIELD_NULL = 0,
		ZDB_DOC_FIELD_INT64,
		ZDB_DOC_FIELD_DOUBLE,
		ZDB_DOC_FIELD_STRING,
		ZDB_DOC_FIELD_BOOL,
		ZDB_DOC_FIELD_BYTES,
		ZDB_DOC_FIELD_OBJECT,
		ZDB_DOC_FIELD_ARRAY,
	} zdb_doc_field_type_t;

	struct zdb_doc;

	typedef union
	{
		int64_t i64;
		double f64;
		const char *str;
		bool b;
		zdb_bytes_t bytes;
		struct zdb_doc *obj;
	} zdb_doc_field_value_t;

	typedef struct
	{
		const char *name;
		zdb_doc_field_type_t type;
		zdb_doc_field_value_t value;
	} zdb_doc_field_t;

	typedef struct zdb_doc
	{
		zdb_t *db;
		const char *collection_name;
		const char *document_id;
		zdb_doc_field_t *fields;
		size_t field_count;
		size_t max_fields;
		uint64_t created_ms;
		uint64_t updated_ms;
		bool valid;
	} zdb_doc_t;

	typedef struct
	{
		const char *field_name;
		zdb_doc_field_type_t type;
		double numeric_value;
		const char *string_value;
		bool bool_value;
	} zdb_doc_query_filter_t;

	typedef struct
	{
		zdb_doc_query_filter_t *filters;
		size_t filter_count;
		uint64_t from_ms;
		uint64_t to_ms;
		uint32_t limit;
	} zdb_doc_query_t;

	typedef struct
	{
		const char *document_id;
		const char *collection_name;
		uint64_t created_ms;
		uint64_t updated_ms;
		uint32_t field_count;
	} zdb_doc_metadata_t;

	zdb_status_t zdb_doc_create(zdb_t *db, const char *collection_name,
								const char *document_id, zdb_doc_t *out_doc);
	zdb_status_t zdb_doc_open(zdb_t *db, const char *collection_name,
							  const char *document_id, zdb_doc_t *out_doc);
	zdb_status_t zdb_doc_save(zdb_doc_t *doc);
	zdb_status_t zdb_doc_delete(zdb_t *db, const char *collection_name,
								const char *document_id);
	zdb_status_t zdb_doc_close(zdb_doc_t *doc);

	zdb_status_t zdb_doc_field_set_i64(zdb_doc_t *doc, const char *field_name,
									   int64_t value);
	zdb_status_t zdb_doc_field_set_f64(zdb_doc_t *doc, const char *field_name,
									   double value);
	zdb_status_t zdb_doc_field_set_string(zdb_doc_t *doc, const char *field_name,
										  const char *value);
	zdb_status_t zdb_doc_field_set_bool(zdb_doc_t *doc, const char *field_name,
										bool value);
	zdb_status_t zdb_doc_field_set_bytes(zdb_doc_t *doc, const char *field_name,
										 const void *value, size_t len);

	zdb_status_t zdb_doc_field_get_i64(const zdb_doc_t *doc, const char *field_name,
									   int64_t *out_value);
	zdb_status_t zdb_doc_field_get_f64(const zdb_doc_t *doc, const char *field_name,
									   double *out_value);
	zdb_status_t zdb_doc_field_get_string(const zdb_doc_t *doc, const char *field_name,
										  const char **out_value);
	zdb_status_t zdb_doc_field_get_bool(const zdb_doc_t *doc, const char *field_name,
										bool *out_value);
	zdb_status_t zdb_doc_field_get_bytes(const zdb_doc_t *doc, const char *field_name,
										 zdb_bytes_t *out_value);

	zdb_status_t zdb_doc_query(zdb_t *db, const zdb_doc_query_t *query,
							   zdb_doc_metadata_t *out_metadata, size_t *out_count);
	zdb_status_t zdb_doc_metadata_free(zdb_doc_metadata_t *metadata, size_t count);

	zdb_status_t zdb_doc_export_flatbuffer(zdb_doc_t *doc, uint8_t *out_buf,
										   size_t out_capacity, size_t *out_len);

#endif /* CONFIG_ZDB_DOC */

/*
 * Shell interface
 */
#if defined(CONFIG_ZDB_SHELL) && (CONFIG_ZDB_SHELL)
	void zdb_shell_register(zdb_t *db);
#endif /* CONFIG_ZDB_SHELL */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYRDB_H_ */
