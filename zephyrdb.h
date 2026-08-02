/**
 * @file zephyrdb.h
 * @brief ZephyrDB public API.
 *
 * Embedded database for Zephyr RTOS: Core + KV + TS + DOC + Eventing +
 * Shell modules. Which declarations are visible depends on the enabled
 * Kconfig options (see the per-module guards below); this generated
 * documentation is built with every module enabled.
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

/**
 * @defgroup zdb_mem Memory helpers
 * @brief Static slab declaration macros for fragmentation-free allocation.
 *
 * ZephyrDB never calls the kernel heap; all runtime allocation comes from
 * memory slabs the application declares at compile time, sized by Kconfig.
 * @{
 */

/** @brief Round @p value up to the next multiple of @p align. */
#define ZDB_ALIGN_UP(value, align) ROUND_UP((value), (align))

/**
 * @brief Declare a static memory slab with build-time size validation.
 *
 * Wraps @c K_MEM_SLAB_DEFINE_STATIC and asserts at compile time that both
 * dimensions are non-zero. The block size is rounded up to @p align.
 *
 * @param name        Slab variable name.
 * @param block_size  Size of one block in bytes (> 0).
 * @param block_count Number of blocks (> 0).
 * @param align       Block alignment in bytes.
 */
#define ZDB_MEM_SLAB_DEFINE(name, block_size, block_count, align)           \
	BUILD_ASSERT((block_size) > 0, "ZephyrDB slab block size must be > 0");   \
	BUILD_ASSERT((block_count) > 0, "ZephyrDB slab block count must be > 0"); \
	K_MEM_SLAB_DEFINE_STATIC(name, ZDB_ALIGN_UP((block_size), (align)), (block_count), (align))

/**
 * @brief Declare the core-context slab, sized by
 *        @c CONFIG_ZDB_CORE_SLAB_BLOCK_SIZE / @c _COUNT.
 * @param name Slab variable name.
 */
#define ZDB_DEFINE_CORE_SLAB(name)                           \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_CORE_SLAB_BLOCK_SIZE, \
											CONFIG_ZDB_CORE_SLAB_BLOCK_COUNT, 4)

/**
 * @brief Declare the cursor slab, sized by
 *        @c CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE / @c _COUNT.
 * @param name Slab variable name.
 */
#define ZDB_DEFINE_CURSOR_SLAB(name)                           \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE, \
											CONFIG_ZDB_CURSOR_SLAB_BLOCK_COUNT, 4)

/**
 * @brief Declare the KV I/O slab, sized by
 *        @c CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE / @c _COUNT.
 *
 * The block size bounds the largest storable KV record
 * (see zdb_kv_set()).
 *
 * @param name Slab variable name.
 */
#define ZDB_DEFINE_KV_IO_SLAB(name)                           \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE, \
											CONFIG_ZDB_KV_IO_SLAB_BLOCK_COUNT, 4)

/**
 * @brief Declare the time-series ingest slab, sized by
 *        @c CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE / @c _COUNT.
 * @param name Slab variable name.
 */
#define ZDB_DEFINE_TS_INGEST_SLAB(name)                           \
	ZDB_MEM_SLAB_DEFINE(name, CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE, \
											CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_COUNT, 4)

/** @} */ /* zdb_mem */

/**
 * @defgroup zdb_core Core
 * @brief Instance lifecycle, status codes, health, telemetry, and the
 *        cursor framework.
 * @{
 */

	/** @brief Status codes returned by every ZephyrDB API. */
	typedef enum
	{
		ZDB_OK = 0,           /**< Success. */
		ZDB_ERR_INVAL,        /**< Invalid argument (NULL pointer, bad length, ...). */
		ZDB_ERR_NOMEM,        /**< Out of slab memory, or payload exceeds a slab block. */
		ZDB_ERR_NOT_FOUND,    /**< Requested record/key/document does not exist. */
		ZDB_ERR_IO,           /**< Backend storage read/write failure. */
		ZDB_ERR_BUSY,         /**< Resource is held (e.g. another TS stream is active). */
		ZDB_ERR_TIMEOUT,      /**< Timed out waiting (e.g. zdb_ts_flush_sync()). */
		ZDB_ERR_UNSUPPORTED,  /**< Operation not supported by the active backend/build. */
		ZDB_ERR_CORRUPT,      /**< Stored data failed integrity checks (CRC, framing). */
		ZDB_ERR_INTERNAL,     /**< Unexpected internal state. */
		ZDB_ERR_COLLISION,    /**< KV record-ID hash collision with a different key (see zdb_kv_set()). */
	} zdb_status_t;

	/** @brief Data-model selector used by cursors and predicates. */
	typedef enum
	{
		ZDB_MODEL_CORE = 0, /**< Core/meta records. */
		ZDB_MODEL_KV,       /**< Key-value records. */
		ZDB_MODEL_TS,       /**< Time-series samples. */
	} zdb_model_t;

	/** @brief Storage backend identifiers. */
	typedef enum
	{
		ZDB_BACKEND_NONE = 0, /**< No backend bound. */
		ZDB_BACKEND_NVS,      /**< Zephyr NVS (KV). */
		ZDB_BACKEND_ZMS,      /**< Zephyr ZMS (KV). */
		ZDB_BACKEND_LFS,      /**< LittleFS file backend (TS, DOC). */
		ZDB_BACKEND_FCB,      /**< Flash Circular Buffer (TS). */
	} zdb_backend_t;

	/** @brief Instance health as reported by zdb_health(). */
	typedef enum
	{
		ZDB_HEALTH_OK = 0,   /**< Fully operational. */
		ZDB_HEALTH_DEGRADED, /**< Operational with recovered/ignored faults. */
		ZDB_HEALTH_READONLY, /**< Writes disabled. */
		ZDB_HEALTH_FAULT,    /**< Not operational. */
	} zdb_health_t;

	/** @brief Immutable byte span (pointer + length). */
	typedef struct
	{
		const uint8_t *data; /**< Pointer to the bytes (not owned). */
		size_t len;          /**< Number of valid bytes at @ref data. */
	} zdb_bytes_t;

	/** @brief Time-series durability counters (see zdb_ts_stats_get()). */
	typedef struct
	{
		uint32_t recover_runs;           /**< Completed zdb_ts_recover_stream() runs. */
		uint32_t recover_failures;       /**< Recovery runs that failed. */
		uint64_t recover_truncated_bytes;/**< Total bytes truncated by recovery. */
		uint32_t crc_failures;           /**< Records dropped for CRC mismatch. */
		uint32_t corrupt_records;        /**< Records dropped for framing corruption. */
		uint32_t unsupported_versions;   /**< Records skipped for unknown format version. */
	} zdb_ts_stats_t;

	/**
	 * @brief Compact, packed telemetry export of ::zdb_ts_stats_t for
	 *        transport or logging.
	 *
	 * Produced by zdb_ts_stats_export(); the @ref crc field protects every
	 * other byte of the struct and is checked by
	 * zdb_ts_stats_export_validate().
	 */
	typedef struct __packed
	{
		uint16_t version;                /**< Export format version. */
		uint16_t reserved;               /**< Reserved, written as zero. */
		uint32_t crc;                    /**< CRC-32 (IEEE) over all bytes except this field. */
		uint32_t recover_runs;           /**< See zdb_ts_stats_t::recover_runs. */
		uint32_t recover_failures;       /**< See zdb_ts_stats_t::recover_failures. */
		uint64_t recover_truncated_bytes;/**< See zdb_ts_stats_t::recover_truncated_bytes. */
		uint32_t crc_failures;           /**< See zdb_ts_stats_t::crc_failures. */
		uint32_t corrupt_records;        /**< See zdb_ts_stats_t::corrupt_records. */
		uint32_t unsupported_versions;   /**< See zdb_ts_stats_t::unsupported_versions. */
	} zdb_ts_stats_export_t;

/** @} */ /* zdb_core */

/** @cond INTERNAL
 * Safe defaults for Kconfig symbols used in struct definitions.
 * These allow compilation in unit tests that bypass the Zephyr module system.
 */
#ifndef CONFIG_ZDB_MAX_KEY_LEN
#define CONFIG_ZDB_MAX_KEY_LEN 48
#endif
/** @endcond */

/**
 * @defgroup zdb_eventing Eventing
 * @brief Synchronous per-operation event notification
 *        (@c CONFIG_ZDB_EVENTING).
 *
 * Listener arrays are registered through ::zdb_cfg_t before zdb_init().
 * Events fire after each operation with the operation's real status
 * (including failures); listener entries with a NULL @c notify are
 * skipped. Callbacks run in the calling thread's context and must not
 * block.
 * @{
 */
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)

	/** @brief KV event kinds. */
	typedef enum
	{
		ZDB_EVENT_KV_SET = 0, /**< A zdb_kv_set() completed. */
		ZDB_EVENT_KV_DELETE,  /**< A zdb_kv_delete() completed. */
	} zdb_event_type_t;

	/** @brief Payload delivered to KV event listeners. */
	typedef struct
	{
		zdb_event_type_t type;                        /**< Operation kind. */
		char namespace_name[CONFIG_ZDB_MAX_KEY_LEN + 1]; /**< Namespace the operation targeted. */
		char key[CONFIG_ZDB_MAX_KEY_LEN + 1];         /**< Key the operation targeted. */
		size_t value_len;                             /**< Value length (set) or 0 (delete). */
		uint64_t timestamp_ms;                        /**< Uptime when the event fired (k_uptime_get()). */
		zdb_status_t status;                          /**< Result of the operation. */
	} zdb_kv_event_t;

	/** @brief KV event listener registration entry. */
	typedef struct
	{
		void (*notify)(const zdb_kv_event_t *event, void *user_ctx); /**< Callback; NULL entries are skipped. */
		void *user_ctx;                                              /**< Opaque pointer passed back to @ref notify. */
	} zdb_event_listener_t;

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)

/** @cond INTERNAL */
#ifndef CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN
#define CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN 24
#endif
/** @endcond */

	/** @brief Time-series event kinds. */
	typedef enum
	{
		ZDB_TS_EVENT_APPEND = 0, /**< A sample append completed. */
		ZDB_TS_EVENT_FLUSH,      /**< A flush completed. */
		ZDB_TS_EVENT_RECOVER,    /**< A zdb_ts_recover_stream() run completed. */
	} zdb_ts_event_type_t;

	/** @brief Payload delivered to time-series event listeners. */
	typedef struct
	{
		zdb_ts_event_type_t type;                                  /**< Operation kind. */
		char stream_name[CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN + 1];   /**< Stream the operation targeted. */
		uint64_t timestamp_ms;   /**< Uptime when the event fired (k_uptime_get()). */
		uint64_t sample_ts_ms;   /**< Appended sample's timestamp (append events). */
		int64_t sample_value;    /**< Appended sample's value (append events). */
		size_t flushed_bytes;    /**< Bytes written (flush events). */
		size_t truncated_bytes;  /**< Bytes truncated (recover events). */
		zdb_status_t status;     /**< Result of the operation. */
	} zdb_ts_event_t;

	/** @brief Time-series event listener registration entry. */
	typedef struct
	{
		void (*notify)(const zdb_ts_event_t *event, void *user_ctx); /**< Callback; NULL entries are skipped. */
		void *user_ctx;                                              /**< Opaque pointer passed back to @ref notify. */
	} zdb_ts_event_listener_t;

#endif /* CONFIG_ZDB_TS */

#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)

	/** @brief Document event kinds. */
	typedef enum
	{
		ZDB_DOC_EVENT_CREATE = 0, /**< A zdb_doc_create() completed. */
		ZDB_DOC_EVENT_SAVE,       /**< A zdb_doc_save() completed. */
		ZDB_DOC_EVENT_DELETE,     /**< A zdb_doc_delete() completed. */
	} zdb_doc_event_type_t;

	/** @brief Payload delivered to document event listeners. */
	typedef struct
	{
		zdb_doc_event_type_t type;                          /**< Operation kind. */
		char collection_name[CONFIG_ZDB_MAX_KEY_LEN + 1];   /**< Collection the operation targeted. */
		char document_id[CONFIG_ZDB_MAX_KEY_LEN + 1];       /**< Document the operation targeted. */
		uint64_t timestamp_ms;    /**< Uptime when the event fired (k_uptime_get()). */
		size_t field_count;       /**< Field count of the document (save events). */
		size_t serialized_bytes;  /**< Serialized size on disk (save events). */
		zdb_status_t status;      /**< Result of the operation. */
	} zdb_doc_event_t;

	/** @brief Document event listener registration entry. */
	typedef struct
	{
		void (*notify)(const zdb_doc_event_t *event, void *user_ctx); /**< Callback; NULL entries are skipped. */
		void *user_ctx;                                               /**< Opaque pointer passed back to @ref notify. */
	} zdb_doc_event_listener_t;

#endif /* CONFIG_ZDB_DOC */

#endif /* CONFIG_ZDB_EVENTING */

/** @} */ /* zdb_eventing */

/**
 * @addtogroup zdb_core
 * @{
 */

	/**
	 * @brief Instance configuration passed to zdb_init().
	 *
	 * The structure (and any listener arrays it references) must outlive
	 * the instance; ZephyrDB keeps the pointer, it does not copy.
	 */
	typedef struct
	{
		const void *kv_backend_fs;     /**< KV backend handle: `struct nvs_fs *` or `struct zms_fs *`, already initialized and mounted by the app. */
		const char *lfs_mount_point;   /**< Mount point for file-backed modules (TS/DOC), e.g. "/lfs"; NULL uses @c CONFIG_ZDB_LFS_MOUNT_POINT. */
		struct k_work_q *work_q;       /**< Work queue for async flushes; NULL uses the system work queue. */
		uint16_t scan_yield_every_n;   /**< Cooperative-scan yield interval in records; 0 disables yielding. */
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
		const zdb_event_listener_t *event_listeners; /**< KV event listener array (may be NULL). */
		size_t event_listener_count;                 /**< Entries in @ref event_listeners. */
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
		const zdb_ts_event_listener_t *ts_event_listeners; /**< TS event listener array (may be NULL). */
		size_t ts_event_listener_count;                    /**< Entries in @ref ts_event_listeners. */
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
		const zdb_doc_event_listener_t *doc_event_listeners; /**< Document event listener array (may be NULL). */
		size_t doc_event_listener_count;                     /**< Entries in @ref doc_event_listeners. */
#endif
#endif
	} zdb_cfg_t;

	/**
	 * @brief Database instance.
	 *
	 * Treat the members as internal state: declare instances with
	 * ZDB_DEFINE_STATIC() (or wire the slab pointers manually) and
	 * initialize with zdb_init() before any other call.
	 */
	typedef struct
	{
		struct k_mutex lock;             /**< Instance lock (internal). */
		struct k_mem_slab *core_slab;    /**< Core-context slab (see ZDB_DEFINE_CORE_SLAB()). */
		struct k_mem_slab *cursor_slab;  /**< Cursor slab (see ZDB_DEFINE_CURSOR_SLAB()). */
		struct k_mem_slab *kv_io_slab;   /**< KV I/O slab; required when KV is enabled. */
		struct k_mem_slab *ts_ingest_slab; /**< TS ingest slab; required when TS is enabled. */
		void *core_ctx;                  /**< Internal core context. */
		void *kv_ctx;                    /**< Internal KV context. */
		void *ts_ctx;                    /**< Internal TS context. */
		const zdb_cfg_t *cfg;            /**< Configuration bound at zdb_init(). */
		zdb_ts_stats_t ts_stats;         /**< Live TS durability counters. */
		zdb_health_t health;             /**< Current health (read via zdb_health()). */
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
		const zdb_event_listener_t *event_listeners; /**< Bound KV listeners (internal). */
		size_t event_listener_count;                 /**< Bound KV listener count (internal). */
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
		const zdb_ts_event_listener_t *ts_event_listeners; /**< Bound TS listeners (internal). */
		size_t ts_event_listener_count;                    /**< Bound TS listener count (internal). */
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
		const zdb_doc_event_listener_t *doc_event_listeners; /**< Bound document listeners (internal). */
		size_t doc_event_listener_count;                     /**< Bound document listener count (internal). */
#endif
#endif
	} zdb_t;

/** @cond INTERNAL */
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
/** @endcond */

/**
 * @brief Declare a static ::zdb_t wired to Kconfig-sized slabs.
 *
 * Declares the core and cursor slabs plus the KV I/O and TS ingest slabs
 * for whichever of those modules are enabled, and a `static zdb_t db_name`
 * pointing at them. Call `zdb_init(&db_name, &cfg)` before first use.
 *
 * @param db_name  Name of the ::zdb_t variable to declare.
 * @param cfg_name Configuration name; currently unused by the macro —
 *                 pass the same ::zdb_cfg_t to zdb_init().
 */
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

	/**
	 * @brief Record filter callback for cursor scans.
	 *
	 * @param model    Data model the record belongs to.
	 * @param record   Raw record bytes (valid only during the call).
	 * @param user_ctx Opaque pointer supplied at cursor open.
	 * @return true to yield the record from zdb_cursor_next(), false to
	 *         skip it.
	 */
	typedef bool (*zdb_predicate_fn)(zdb_model_t model, const zdb_bytes_t *record, void *user_ctx);

	/**
	 * @brief Cursor for cooperative scans (currently opened via
	 *        zdb_ts_cursor_open()).
	 *
	 * Treat the members as internal; drive the cursor with
	 * zdb_cursor_next(), zdb_cursor_reset(), and zdb_cursor_close().
	 */
	typedef struct zdb_cursor
	{
		zdb_model_t model;        /**< Data model being scanned. */
		zdb_backend_t backend;    /**< Backend the scan reads from. */
		uint32_t flags;           /**< Internal flags. */
		uint32_t iter_count;      /**< Records yielded so far. */
		uint64_t position;        /**< Internal scan position. */
		zdb_bytes_t current;      /**< Most recently yielded record. */
		zdb_predicate_fn predicate; /**< Optional filter; NULL yields everything. */
		void *predicate_ctx;      /**< Opaque pointer passed to @ref predicate. */
		void *impl;               /**< Backend-specific state (internal). */
	} zdb_cursor_t;

	/**
	 * @brief Initialize a database instance.
	 *
	 * Binds @p cfg (kept by reference — it must outlive the instance),
	 * validates the slab wiring for the enabled modules, and readies the
	 * instance for module opens.
	 *
	 * @param db  Instance declared via ZDB_DEFINE_STATIC() or manually wired.
	 * @param cfg Configuration; see ::zdb_cfg_t for backend requirements.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p db or @p cfg is NULL, or a required slab is missing.
	 */
	zdb_status_t zdb_init(zdb_t *db, const zdb_cfg_t *cfg);

	/**
	 * @brief Tear down an instance and release all module state.
	 *
	 * Closes any active TS stream and frees internal contexts back to the
	 * slabs. The instance may be re-initialized with zdb_init() afterwards.
	 *
	 * @param db Initialized instance.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p db is NULL or not initialized.
	 */
	zdb_status_t zdb_deinit(zdb_t *db);

	/**
	 * @brief Report the instance's current health.
	 *
	 * @param db Initialized instance.
	 * @return Current ::zdb_health_t; ZDB_HEALTH_FAULT if @p db is NULL or
	 *         uninitialized.
	 */
	zdb_health_t zdb_health(const zdb_t *db);

	/**
	 * @brief Return a short printable name for a status code.
	 *
	 * @param status Any ::zdb_status_t value.
	 * @return Static string such as "OK" or "COLLISION"; never NULL.
	 */
	const char *zdb_status_str(zdb_status_t status);

	/**
	 * @brief Copy the live time-series durability counters.
	 *
	 * @param db        Initialized instance.
	 * @param out_stats Destination; zeroed when @p db is NULL/uninitialized.
	 */
	void zdb_ts_stats_get(const zdb_t *db, zdb_ts_stats_t *out_stats);

	/**
	 * @brief Reset the time-series durability counters to zero.
	 *
	 * @param db Initialized instance (NULL is ignored).
	 */
	void zdb_ts_stats_reset(zdb_t *db);

	/**
	 * @brief Fill a packed, CRC-protected telemetry export of the TS stats.
	 *
	 * The CRC covers every byte of the record except the crc field itself,
	 * so any tampering or transport corruption is detected by
	 * zdb_ts_stats_export_validate().
	 *
	 * @param db         Initialized instance.
	 * @param out_export Destination record.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p db or @p out_export is NULL.
	 */
	zdb_status_t zdb_ts_stats_export(const zdb_t *db, zdb_ts_stats_export_t *out_export);

	/**
	 * @brief Validate a received telemetry export record.
	 *
	 * @param export_data Record to check.
	 * @retval ZDB_OK              CRC and version are valid.
	 * @retval ZDB_ERR_INVAL       @p export_data is NULL.
	 * @retval ZDB_ERR_CORRUPT     CRC mismatch.
	 * @retval ZDB_ERR_UNSUPPORTED Unknown format version.
	 */
	zdb_status_t zdb_ts_stats_export_validate(const zdb_ts_stats_export_t *export_data);

	/**
	 * @brief Rewind a cursor to the start of its scan.
	 *
	 * @param cursor Open cursor.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p cursor is NULL or not open.
	 */
	zdb_status_t zdb_cursor_reset(zdb_cursor_t *cursor);

	/**
	 * @brief Close a cursor and release its slab allocation.
	 *
	 * @param cursor Open cursor; safe to call once per open.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p cursor is NULL or not open.
	 */
	zdb_status_t zdb_cursor_close(zdb_cursor_t *cursor);

/** @} */ /* zdb_core */

/**
 * @defgroup zdb_kv Key-Value
 * @brief Namespaced key-value storage on NVS or ZMS (@c CONFIG_ZDB_KV).
 *
 * Records are persisted in the v2 on-disk format
 * `[0xDB tag][ns_len][key_len][namespace][key][value]` under a backend
 * record ID hashed (FNV-1a) from the namespace and key together, so
 * namespaces are isolated in storage and every read verifies the full
 * identity of the stored record. Records written by pre-v2 builds are
 * treated as absent and their slots are reclaimed on the next set.
 * @{
 */
#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
	/** @brief Open handle to one KV namespace (see zdb_kv_open()). */
	typedef struct
	{
		zdb_t *db;                  /**< Owning instance. */
		const char *namespace_name; /**< Namespace bound at open (kept by reference). */
	} zdb_kv_t;

	/** @brief KV iterator state (see zdb_kv_iter_open()). */
	typedef struct
	{
		zdb_kv_t *kv;    /**< Namespace being iterated. */
		size_t position; /**< Internal iteration position. */
		void *impl;      /**< Internal state. */
	} zdb_kv_iter_t;

	/**
	 * @brief Open a namespace handle.
	 *
	 * @param kv             Handle to fill.
	 * @param db             Initialized instance with a KV backend configured.
	 * @param namespace_name Namespace string, at most @c CONFIG_ZDB_MAX_KEY_LEN
	 *                       bytes; kept by reference and must outlive @p kv.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument or over-long namespace.
	 */
	zdb_status_t zdb_kv_open(zdb_t *db, const char *namespace_name, zdb_kv_t *kv);

	/**
	 * @brief Close a namespace handle.
	 *
	 * @param kv Open handle.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p kv is NULL or not open.
	 */
	zdb_status_t zdb_kv_close(zdb_kv_t *kv);

	/**
	 * @brief Store (create or overwrite) a value under a key.
	 *
	 * The maximum value size is
	 * `CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE - 3 - strlen(namespace) - strlen(key)`.
	 *
	 * @param kv        Open namespace handle.
	 * @param key       Key string, at most @c CONFIG_ZDB_MAX_KEY_LEN bytes.
	 * @param value     Value bytes; may be NULL when @p value_len is 0.
	 * @param value_len Value length in bytes; 0 stores an empty value.
	 * @retval ZDB_OK            Success (including a byte-identical rewrite).
	 * @retval ZDB_ERR_INVAL     NULL argument or over-long key.
	 * @retval ZDB_ERR_NOMEM     Record exceeds the KV I/O slab block size.
	 * @retval ZDB_ERR_COLLISION The key's backend record ID is occupied by a
	 *                           different (namespace, key); the stored record
	 *                           is untouched — choose a different key name.
	 * @retval ZDB_ERR_IO        Backend write failure.
	 */
	zdb_status_t zdb_kv_set(zdb_kv_t *kv, const char *key, const void *value, size_t value_len);

	/**
	 * @brief Read the value stored under a key.
	 *
	 * Never fails on a too-small buffer: copies up to @p out_capacity bytes
	 * and reports the full stored length in @p out_len — compare the two to
	 * detect truncation.
	 *
	 * @param kv           Open namespace handle.
	 * @param key          Key string.
	 * @param out_value    Destination buffer (may be NULL if @p out_capacity is 0).
	 * @param out_capacity Destination capacity in bytes.
	 * @param out_len      Set to the full stored value length.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument or over-long key.
	 * @retval ZDB_ERR_NOT_FOUND No record for this (namespace, key), including
	 *                           slots holding a different key or pre-v2 data.
	 * @retval ZDB_ERR_IO        Backend read failure.
	 */
	zdb_status_t zdb_kv_get(zdb_kv_t *kv, const char *key, void *out_value,
							size_t out_capacity, size_t *out_len);

	/**
	 * @brief Store a NUL-terminated string.
	 *
	 * Convenience wrapper over zdb_kv_set() that stores `strlen(value) + 1`
	 * bytes, so the terminator is part of the stored value. An empty string
	 * is stored as a single NUL byte.
	 *
	 * @param kv    Open namespace handle.
	 * @param key   Key string.
	 * @param value NUL-terminated value to store.
	 * @retval ZDB_OK             Success.
	 * @retval ZDB_ERR_INVAL      NULL argument or over-long key.
	 * @retval ZDB_ERR_NOMEM      Record exceeds the KV IO slab block size, or
	 *                            no slab block was free.
	 * @retval ZDB_ERR_COLLISION  The record ID slot holds a different key.
	 * @retval ZDB_ERR_IO         Backend write failure.
	 */
	zdb_status_t zdb_kv_set_str(zdb_kv_t *kv, const char *key, const char *value);

	/**
	 * @brief Read a value as a NUL-terminated string.
	 *
	 * Never fails on a too-small buffer and always terminates @p out_str,
	 * copying at most `out_capacity - 1` bytes. Detect truncation with
	 * `*out_len + 1 > out_capacity`.
	 *
	 * Values written by zdb_kv_set_str() round-trip exactly. A value written
	 * through zdb_kv_set() without a terminator is also returned as a
	 * terminated string.
	 *
	 * @param kv           Open namespace handle.
	 * @param key          Key string.
	 * @param out_str      Destination buffer; must be at least 1 byte.
	 * @param out_capacity Destination capacity in bytes, including the terminator.
	 * @param out_len      Set to the string length excluding the terminator. If
	 *                     the value did not fit, set to the stored byte count,
	 *                     which bounds the buffer size needed.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument, zero capacity, or over-long key.
	 * @retval ZDB_ERR_NOT_FOUND No record for this (namespace, key).
	 * @retval ZDB_ERR_IO        Backend read failure.
	 */
	zdb_status_t zdb_kv_get_str(zdb_kv_t *kv, const char *key, char *out_str,
								size_t out_capacity, size_t *out_len);

	/**
	 * @brief Delete the record stored under a key.
	 *
	 * Verifies the stored record's identity first, so a colliding record
	 * belonging to a different (namespace, key) is never deleted.
	 *
	 * @param kv  Open namespace handle.
	 * @param key Key string.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument or over-long key.
	 * @retval ZDB_ERR_NOT_FOUND No record for this (namespace, key).
	 * @retval ZDB_ERR_IO        Backend delete failure.
	 */
	zdb_status_t zdb_kv_delete(zdb_kv_t *kv, const char *key);

	/**
	 * @brief Open an iterator over the namespace's keys.
	 *
	 * The iterator walks keys set/deleted during the current session (RAM
	 * index, 128 entries); cross-session iteration from persisted data is
	 * not yet implemented.
	 *
	 * @param kv       Open namespace handle.
	 * @param out_iter Iterator to initialize.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument.
	 */
	zdb_status_t zdb_kv_iter_open(zdb_kv_t *kv, zdb_kv_iter_t *out_iter);

	/**
	 * @brief Fetch the next key (and its value) from an iterator.
	 *
	 * Both copies clamp to their capacity while reporting full lengths,
	 * following the zdb_kv_get() truncation contract.
	 *
	 * @param iter               Open iterator.
	 * @param out_key            Buffer for the NUL-terminated key.
	 * @param out_key_capacity   Capacity of @p out_key in bytes.
	 * @param out_key_len        Set to the full key length.
	 * @param out_value          Buffer for the value (may be NULL if capacity 0).
	 * @param out_value_capacity Capacity of @p out_value in bytes.
	 * @param out_value_len      Set to the full stored value length.
	 * @retval ZDB_OK            A record was produced.
	 * @retval ZDB_ERR_NOT_FOUND Iteration is exhausted.
	 * @retval ZDB_ERR_INVAL     NULL argument or iterator not open.
	 */
	zdb_status_t zdb_kv_iter_next(zdb_kv_iter_t *iter, char *out_key,
							   size_t out_key_capacity, size_t *out_key_len,
							   void *out_value, size_t out_value_capacity,
							   size_t *out_value_len);

	/**
	 * @brief Close an iterator.
	 *
	 * @param iter Open iterator.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p iter is NULL or not open.
	 */
	zdb_status_t zdb_kv_iter_close(zdb_kv_iter_t *iter);
#endif /* CONFIG_ZDB_KV */

/** @} */ /* zdb_kv */

/**
 * @defgroup zdb_ts Time-series
 * @brief Append-oriented int64 sample streams on LittleFS or FCB
 *        (@c CONFIG_ZDB_TS).
 *
 * One instance handles **one active stream** at a time: opening a second,
 * different stream returns ::ZDB_ERR_BUSY until zdb_deinit(); re-opening
 * the active stream is allowed. Appends land in a RAM ingest buffer and
 * reach storage on flush (with the FCB backend appends are written
 * through synchronously and flushes are no-ops).
 * @{
 */
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	/** @brief Open handle to one time-series stream (see zdb_ts_open()). */
	typedef struct
	{
		zdb_t *db;               /**< Owning instance. */
		const char *stream_name; /**< Stream bound at open (kept by reference). */
	} zdb_ts_t;

	/** @brief One int64 sample: timestamp + value. */
	typedef struct
	{
		uint64_t ts_ms; /**< Sample timestamp in milliseconds (application-defined epoch). */
		int64_t value;  /**< Sample value. */
	} zdb_ts_sample_i64_t;

	/** @brief Aggregation functions for zdb_ts_query_aggregate(). */
	typedef enum
	{
		ZDB_TS_AGG_MIN = 0, /**< Minimum sample value. */
		ZDB_TS_AGG_MAX,     /**< Maximum sample value. */
		ZDB_TS_AGG_AVG,     /**< Arithmetic mean of sample values. */
		ZDB_TS_AGG_SUM,     /**< Sum of sample values. */
		ZDB_TS_AGG_COUNT,   /**< Number of samples in the window. */
	} zdb_ts_agg_t;

	/** @brief Inclusive timestamp window for queries and cursors. */
	typedef struct
	{
		uint64_t from_ts_ms; /**< Window start (inclusive). */
		uint64_t to_ts_ms;   /**< Window end (inclusive). */
	} zdb_ts_window_t;

/** @brief A ::zdb_ts_window_t covering all possible timestamps. */
#define ZDB_TS_WINDOW_ALL ((zdb_ts_window_t){.from_ts_ms = 0, .to_ts_ms = UINT64_MAX})

	/** @brief Result of zdb_ts_query_aggregate(). */
	typedef struct
	{
		zdb_ts_agg_t agg; /**< Aggregation that was computed. */
		double value;     /**< Aggregate value (equals @ref points for COUNT). */
		uint32_t points;  /**< Number of samples that matched the window. */
		/**
		 * @brief True when the scan stopped early and @ref value is partial.
		 *
		 * Set when the scan reached @c CONFIG_ZDB_TS_MAX_AGG_POINTS while
		 * matching samples remained. Never set for ::ZDB_TS_AGG_COUNT,
		 * which is not capped.
		 */
		bool truncated;
	} zdb_ts_agg_result_t;

	/**
	 * @brief Open (or re-open) the instance's active stream.
	 *
	 * @param ts          Handle to fill.
	 * @param db          Initialized instance.
	 * @param stream_name Stream name, at most
	 *                    @c CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN bytes; kept by
	 *                    reference and must outlive @p ts.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument or over-long stream name.
	 * @retval ZDB_ERR_BUSY  A different stream is already active on @p db
	 *                       (until zdb_deinit()).
	 */
	zdb_status_t zdb_ts_open(zdb_t *db, const char *stream_name, zdb_ts_t *ts);

	/**
	 * @brief Close a stream handle.
	 *
	 * Note: the instance's active-stream binding persists until
	 * zdb_deinit(); closing does not allow a different stream to open.
	 *
	 * @param ts Open stream handle.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p ts is NULL or not open.
	 */
	zdb_status_t zdb_ts_close(zdb_ts_t *ts);

	/**
	 * @brief Append one sample to the stream's ingest buffer.
	 *
	 * @param ts     Open stream handle.
	 * @param sample Sample to append.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument.
	 * @retval ZDB_ERR_NOMEM Ingest buffer full and flush could not make room.
	 * @retval ZDB_ERR_IO    Backend write failure (write-through backends).
	 */
	zdb_status_t zdb_ts_append_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *sample);

	/**
	 * @brief Append a batch of samples.
	 *
	 * @param ts           Open stream handle.
	 * @param samples      Sample array.
	 * @param sample_count Number of samples in @p samples.
	 * @retval ZDB_OK        All samples appended.
	 * @retval ZDB_ERR_INVAL NULL argument or zero count.
	 * @retval ZDB_ERR_NOMEM Ran out of ingest space part-way.
	 * @retval ZDB_ERR_IO    Backend write failure (write-through backends).
	 */
	zdb_status_t zdb_ts_append_batch_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *samples,
										 size_t sample_count);

	/**
	 * @brief Queue an asynchronous flush of the ingest buffer.
	 *
	 * The flush runs on the configured work queue (zdb_cfg_t::work_q or
	 * the system work queue). A no-op returning ::ZDB_OK on write-through
	 * backends (FCB).
	 *
	 * @param ts Open stream handle.
	 * @retval ZDB_OK        Flush queued (or nothing to flush).
	 * @retval ZDB_ERR_INVAL @p ts is NULL or not open.
	 */
	zdb_status_t zdb_ts_flush_async(zdb_ts_t *ts);

	/**
	 * @brief Flush the ingest buffer and wait for completion.
	 *
	 * A no-op returning ::ZDB_OK on write-through backends (FCB).
	 *
	 * @param ts      Open stream handle.
	 * @param timeout How long to wait; @c K_NO_WAIT returns immediately.
	 * @retval ZDB_OK          Flush completed.
	 * @retval ZDB_ERR_INVAL   @p ts is NULL or not open.
	 * @retval ZDB_ERR_BUSY    @c K_NO_WAIT and the flush lock is held.
	 * @retval ZDB_ERR_TIMEOUT Timed out waiting for the flush.
	 * @retval ZDB_ERR_IO      Backend write failure.
	 */
	zdb_status_t zdb_ts_flush_sync(zdb_ts_t *ts, k_timeout_t timeout);

	/**
	 * @brief Compute an aggregate over samples in a time window.
	 *
	 * Covers flushed records plus samples still in the ingest buffer.
	 *
	 * ::ZDB_TS_AGG_MIN, ::ZDB_TS_AGG_MAX, ::ZDB_TS_AGG_AVG and
	 * ::ZDB_TS_AGG_SUM scan at most @c CONFIG_ZDB_TS_MAX_AGG_POINTS samples;
	 * if more matched, the result is computed from that prefix and
	 * zdb_ts_agg_result_t::truncated is set. They report
	 * ::ZDB_ERR_NOT_FOUND when nothing matched, since they have no value to
	 * return.
	 *
	 * ::ZDB_TS_AGG_COUNT is neither capped nor truncated, and reports an
	 * empty window as ::ZDB_OK with `points == 0`. Counting every sample in
	 * the stream (an unbounded @p window) reads no payloads.
	 *
	 * @param ts         Open stream handle.
	 * @param window     Inclusive timestamp window (::ZDB_TS_WINDOW_ALL for everything).
	 * @param agg        Aggregation function.
	 * @param out_result Result; `points == 0` means no samples matched.
	 * @retval ZDB_OK              Success.
	 * @retval ZDB_ERR_INVAL       NULL argument or unknown @p agg.
	 * @retval ZDB_ERR_NOT_FOUND   No samples matched (value-bearing aggregates only).
	 * @retval ZDB_ERR_UNSUPPORTED Backend cannot aggregate (FCB).
	 * @retval ZDB_ERR_IO          Backend read failure.
	 */
	zdb_status_t zdb_ts_query_aggregate(zdb_ts_t *ts, zdb_ts_window_t window,
										zdb_ts_agg_t agg, zdb_ts_agg_result_t *out_result);

	/**
	 * @brief Scan the stream's storage and truncate a corrupt tail.
	 *
	 * Walks the on-disk records, drops anything failing CRC/framing checks,
	 * and updates the instance's ::zdb_ts_stats_t counters. A no-op
	 * reporting zero truncated bytes on write-through backends (FCB).
	 *
	 * @param ts                  Open stream handle.
	 * @param out_truncated_bytes Set to the number of bytes truncated (may be NULL).
	 * @retval ZDB_OK        Recovery completed (even if bytes were dropped).
	 * @retval ZDB_ERR_INVAL @p ts is NULL or not open.
	 * @retval ZDB_ERR_IO    Backend read/write failure during recovery.
	 */
	zdb_status_t zdb_ts_recover_stream(zdb_ts_t *ts, size_t *out_truncated_bytes);

	/**
	 * @brief Open a cursor over the stream's samples.
	 *
	 * The cursor iterates flushed records from storage plus samples still
	 * in the RAM ingest buffer, filtered by @p window and the optional
	 * @p predicate.
	 *
	 * @param ts            Open stream handle.
	 * @param window        Inclusive timestamp window.
	 * @param predicate     Optional filter; NULL yields every sample.
	 * @param predicate_ctx Opaque pointer passed to @p predicate.
	 * @param out_cursor    Cursor to initialize (close with zdb_cursor_close()).
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument.
	 * @retval ZDB_ERR_NOMEM Cursor slab exhausted.
	 */
	zdb_status_t zdb_ts_cursor_open(zdb_ts_t *ts, zdb_ts_window_t window,
									zdb_predicate_fn predicate, void *predicate_ctx,
									zdb_cursor_t *out_cursor);

	/**
	 * @brief Fetch the next record from a cursor.
	 *
	 * Part of the core cursor framework; declared with the TS module
	 * because TS is currently the only producer of cursors.
	 *
	 * @param cursor     Open cursor.
	 * @param out_record Set to the next record's bytes; the data stays
	 *                   valid until the next cursor call.
	 * @retval ZDB_OK            A record was produced.
	 * @retval ZDB_ERR_NOT_FOUND Scan is exhausted.
	 * @retval ZDB_ERR_INVAL     NULL argument or cursor not open.
	 * @retval ZDB_ERR_IO        Backend read failure.
	 */
	zdb_status_t zdb_cursor_next(zdb_cursor_t *cursor, zdb_bytes_t *out_record);
#endif /* CONFIG_ZDB_TS */

/**
 * @brief Serialize one sample as a FlatBuffer
 *        (@c CONFIG_ZDB_FLATBUFFERS + @c CONFIG_FLATCC).
 *
 * @param sample       Sample to serialize.
 * @param out_buf      Destination buffer.
 * @param out_capacity Capacity of @p out_buf in bytes.
 * @param out_len      Set to the serialized size.
 * @retval ZDB_OK            Success.
 * @retval ZDB_ERR_INVAL     NULL argument.
 * @retval ZDB_ERR_NOMEM     @p out_buf is too small (required size in @p out_len).
 * @retval ZDB_ERR_UNSUPPORTED Built without FlatCC support.
 */
#if defined(CONFIG_ZDB_FLATBUFFERS) && (CONFIG_ZDB_FLATBUFFERS)
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	zdb_status_t zdb_ts_sample_i64_export_flatbuffer(const zdb_ts_sample_i64_t *sample,
													 uint8_t *out_buf, size_t out_capacity,
													 size_t *out_len);
#endif
#endif /* CONFIG_ZDB_FLATBUFFERS */

/** @} */ /* zdb_ts */

/**
 * @defgroup zdb_doc Document model
 * @brief Typed-field documents in collections on a mounted filesystem
 *        (@c CONFIG_ZDB_DOC, requires @c CONFIG_FILE_SYSTEM).
 *
 * Documents are persisted one file per document under the configured
 * mount point. Implemented field types are INT64, DOUBLE, STRING, BOOL,
 * and BYTES; NULL, OBJECT, and ARRAY appear in the enum but are not
 * implemented and return ::ZDB_ERR_UNSUPPORTED.
 * @{
 */
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)

	/** @brief Document field types. */
	typedef enum
	{
		ZDB_DOC_FIELD_NULL = 0, /**< Reserved; not implemented. */
		ZDB_DOC_FIELD_INT64,    /**< int64_t value. */
		ZDB_DOC_FIELD_DOUBLE,   /**< double value. */
		ZDB_DOC_FIELD_STRING,   /**< NUL-terminated string value. */
		ZDB_DOC_FIELD_BOOL,     /**< bool value. */
		ZDB_DOC_FIELD_BYTES,    /**< Raw byte span value. */
		ZDB_DOC_FIELD_OBJECT,   /**< Reserved; not implemented. */
		ZDB_DOC_FIELD_ARRAY,    /**< Reserved; not implemented. */
	} zdb_doc_field_type_t;

	struct zdb_doc;

	/** @brief Field value storage; the active member follows ::zdb_doc_field_type_t. */
	typedef union
	{
		int64_t i64;         /**< ::ZDB_DOC_FIELD_INT64. */
		double f64;          /**< ::ZDB_DOC_FIELD_DOUBLE. */
		const char *str;     /**< ::ZDB_DOC_FIELD_STRING. */
		bool b;              /**< ::ZDB_DOC_FIELD_BOOL. */
		zdb_bytes_t bytes;   /**< ::ZDB_DOC_FIELD_BYTES. */
		struct zdb_doc *obj; /**< ::ZDB_DOC_FIELD_OBJECT (reserved). */
	} zdb_doc_field_value_t;

	/** @brief One named, typed document field. */
	typedef struct
	{
		const char *name;            /**< Field name. */
		zdb_doc_field_type_t type;   /**< Value type. */
		zdb_doc_field_value_t value; /**< Value (active member per @ref type). */
	} zdb_doc_field_t;

	/**
	 * @brief In-memory document handle.
	 *
	 * Obtained from zdb_doc_create()/zdb_doc_open(); treat the members as
	 * read-only and mutate fields through the zdb_doc_field_set_* /
	 * zdb_doc_field_get_* accessors.
	 */
	typedef struct zdb_doc
	{
		zdb_t *db;                   /**< Owning instance. */
		const char *collection_name; /**< Collection this document belongs to. */
		const char *document_id;     /**< Document identifier. */
		zdb_doc_field_t *fields;     /**< Field array (internal storage). */
		size_t field_count;          /**< Number of populated fields. */
		size_t max_fields;           /**< Field capacity. */
		uint64_t created_ms;         /**< Creation time, milliseconds since boot. */
		uint64_t updated_ms;         /**< Last-save time, milliseconds since boot. */
		bool valid;                  /**< True while the handle is open. */
	} zdb_doc_t;

	/** @brief One equality filter for zdb_doc_query(); filters AND-combine. */
	typedef struct
	{
		const char *field_name;    /**< Field to match. */
		zdb_doc_field_type_t type; /**< Expected field type. */
		double numeric_value;      /**< Match value for INT64/DOUBLE fields. */
		const char *string_value;  /**< Match value for STRING fields. */
		bool bool_value;           /**< Match value for BOOL fields. */
	} zdb_doc_query_filter_t;

	/** @brief Query description for zdb_doc_query(). */
	typedef struct
	{
		zdb_doc_query_filter_t *filters; /**< Filter array (AND-combined); may be NULL. */
		size_t filter_count;             /**< Entries in @ref filters. */
		uint64_t from_ms;                /**< Window start on @c updated_ms (uptime-based); 0 = unbounded. */
		uint64_t to_ms;                  /**< Window end on @c updated_ms; 0 = unbounded. */
		uint32_t limit;                  /**< Maximum matches to report; 0 = unlimited. */
	} zdb_doc_query_t;

	/** @brief Per-document metadata produced by zdb_doc_query(). */
	typedef struct
	{
		const char *document_id;     /**< Matched document's ID (heap-backed; free via zdb_doc_metadata_free()). */
		const char *collection_name; /**< Collection containing the match. */
		uint64_t created_ms;         /**< Creation time, milliseconds since boot. */
		uint64_t updated_ms;         /**< Last-save time, milliseconds since boot. */
		uint32_t field_count;        /**< Number of fields in the document. */
	} zdb_doc_metadata_t;

	/** @brief One (collection, document) pair in a collection manifest. */
	typedef struct
	{
		const char *collection_name; /**< Collection name. */
		const char *document_id;     /**< Document identifier. */
	} zdb_doc_manifest_entry_t;

	/**
	 * @brief Create a new, empty document.
	 *
	 * The document exists on storage only after zdb_doc_save().
	 *
	 * @param db              Initialized instance with DOC enabled.
	 * @param collection_name Collection name, at most @c CONFIG_ZDB_MAX_KEY_LEN bytes.
	 * @param document_id     Document ID, at most @c CONFIG_ZDB_MAX_KEY_LEN bytes.
	 * @param out_doc         Handle to fill; close with zdb_doc_close().
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument, over-long name, or name containing
	 *                       path separators.
	 * @retval ZDB_ERR_NOMEM Document slots exhausted.
	 */
	zdb_status_t zdb_doc_create(zdb_t *db, const char *collection_name,
								const char *document_id, zdb_doc_t *out_doc);

	/**
	 * @brief Load an existing document from storage.
	 *
	 * The stored CRC covers the header and every field payload, so a flipped
	 * payload byte or a truncated file is reported rather than returned as
	 * field values. Documents written by older versions (header-only CRC) are
	 * still accepted.
	 *
	 * If the document file is missing but zdb_doc_save() left a complete
	 * staging file behind, that file is promoted and opened.
	 *
	 * @param db              Initialized instance with DOC enabled.
	 * @param collection_name Collection name.
	 * @param document_id     Document ID.
	 * @param out_doc         Handle to fill; close with zdb_doc_close().
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument, over-long name, or path separators.
	 * @retval ZDB_ERR_NOT_FOUND No such document.
	 * @retval ZDB_ERR_CORRUPT   Stored document failed its CRC or is truncated.
	 * @retval ZDB_ERR_NOMEM     Stored document has more fields than the handle holds.
	 * @retval ZDB_ERR_UNSUPPORTED Stored document uses an unimplemented field type.
	 * @retval ZDB_ERR_IO        Filesystem read failure.
	 */
	zdb_status_t zdb_doc_open(zdb_t *db, const char *collection_name,
							  const char *document_id, zdb_doc_t *out_doc);

	/**
	 * @brief Persist a document's current fields.
	 *
	 * Atomic: the document is serialized into a staging file and renamed over
	 * the stored one, so an interrupted save leaves the previously stored
	 * document intact. The CRC written covers the header and every field
	 * payload. Updates zdb_doc_t::updated_ms.
	 *
	 * @param doc Open document handle.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p doc is NULL or not open.
	 * @retval ZDB_ERR_UNSUPPORTED A field uses an unimplemented type.
	 * @retval ZDB_ERR_IO    Filesystem write or rename failure.
	 */
	zdb_status_t zdb_doc_save(zdb_doc_t *doc);

	/**
	 * @brief Delete a document from storage.
	 *
	 * @param db              Initialized instance with DOC enabled.
	 * @param collection_name Collection name.
	 * @param document_id     Document ID.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument, over-long name, or path separators.
	 * @retval ZDB_ERR_NOT_FOUND No such document.
	 * @retval ZDB_ERR_IO        Filesystem delete failure.
	 */
	zdb_status_t zdb_doc_delete(zdb_t *db, const char *collection_name,
								const char *document_id);

	/**
	 * @brief Close a document handle and release its in-memory fields.
	 *
	 * Unsaved field changes are discarded.
	 *
	 * @param doc Open document handle.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p doc is NULL or not open.
	 */
	zdb_status_t zdb_doc_close(zdb_doc_t *doc);

	/**
	 * @brief Set (create or replace) an INT64 field.
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param value      Value to store.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument.
	 * @retval ZDB_ERR_NOMEM Field capacity exhausted.
	 */
	zdb_status_t zdb_doc_field_set_i64(zdb_doc_t *doc, const char *field_name,
									   int64_t value);
	/**
	 * @brief Set (create or replace) a DOUBLE field.
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param value      Value to store.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument.
	 * @retval ZDB_ERR_NOMEM Field capacity exhausted.
	 */
	zdb_status_t zdb_doc_field_set_f64(zdb_doc_t *doc, const char *field_name,
									   double value);
	/**
	 * @brief Set (create or replace) a STRING field.
	 *
	 * The string is copied into the document's field storage.
	 *
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param value      NUL-terminated string to store.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument.
	 * @retval ZDB_ERR_NOMEM Field capacity exhausted.
	 */
	zdb_status_t zdb_doc_field_set_string(zdb_doc_t *doc, const char *field_name,
										  const char *value);
	/**
	 * @brief Set (create or replace) a BOOL field.
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param value      Value to store.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument.
	 * @retval ZDB_ERR_NOMEM Field capacity exhausted.
	 */
	zdb_status_t zdb_doc_field_set_bool(zdb_doc_t *doc, const char *field_name,
										bool value);
	/**
	 * @brief Set (create or replace) a BYTES field.
	 *
	 * The bytes are copied into the document's field storage.
	 *
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param value      Bytes to store.
	 * @param len        Number of bytes at @p value.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL NULL argument.
	 * @retval ZDB_ERR_NOMEM Field capacity exhausted.
	 */
	zdb_status_t zdb_doc_field_set_bytes(zdb_doc_t *doc, const char *field_name,
										 const void *value, size_t len);

	/**
	 * @brief Read an INT64 field.
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param out_value  Set to the field's value.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument or the field has a different type.
	 * @retval ZDB_ERR_NOT_FOUND No field with this name.
	 */
	zdb_status_t zdb_doc_field_get_i64(const zdb_doc_t *doc, const char *field_name,
									   int64_t *out_value);
	/**
	 * @brief Read a DOUBLE field.
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param out_value  Set to the field's value.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument or the field has a different type.
	 * @retval ZDB_ERR_NOT_FOUND No field with this name.
	 */
	zdb_status_t zdb_doc_field_get_f64(const zdb_doc_t *doc, const char *field_name,
									   double *out_value);
	/**
	 * @brief Read a STRING field.
	 *
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param out_value  Set to the stored string; valid until the field is
	 *                   modified or the document is closed.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument or the field has a different type.
	 * @retval ZDB_ERR_NOT_FOUND No field with this name.
	 */
	zdb_status_t zdb_doc_field_get_string(const zdb_doc_t *doc, const char *field_name,
										  const char **out_value);
	/**
	 * @brief Read a BOOL field.
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param out_value  Set to the field's value.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument or the field has a different type.
	 * @retval ZDB_ERR_NOT_FOUND No field with this name.
	 */
	zdb_status_t zdb_doc_field_get_bool(const zdb_doc_t *doc, const char *field_name,
										bool *out_value);
	/**
	 * @brief Read a BYTES field.
	 *
	 * @param doc        Open document handle.
	 * @param field_name Field name.
	 * @param out_value  Set to the stored bytes; valid until the field is
	 *                   modified or the document is closed.
	 * @retval ZDB_OK            Success.
	 * @retval ZDB_ERR_INVAL     NULL argument or the field has a different type.
	 * @retval ZDB_ERR_NOT_FOUND No field with this name.
	 */
	zdb_status_t zdb_doc_field_get_bytes(const zdb_doc_t *doc, const char *field_name,
										 zdb_bytes_t *out_value);

	/**
	 * @brief Query document metadata across all collections.
	 *
	 * Filters are AND-combined equality matches; the time window
	 * (zdb_doc_query_t::from_ms / @c to_ms) filters on @c updated_ms,
	 * which is milliseconds since boot, not wall-clock time. There is no
	 * collection filter — the scan covers every collection.
	 *
	 * @param db           Initialized instance with DOC enabled.
	 * @param query        Query description.
	 * @param out_metadata Result array, or NULL to only count matches.
	 *                     Populated entries must be released with
	 *                     zdb_doc_metadata_free().
	 * @param out_count    In: capacity of @p out_metadata. Out: number of
	 *                     matches reported (total match count when
	 *                     @p out_metadata is NULL).
	 * @retval ZDB_OK        Success (zero matches is still ::ZDB_OK).
	 * @retval ZDB_ERR_INVAL NULL @p db, @p query, or @p out_count.
	 * @retval ZDB_ERR_IO    Filesystem scan failure.
	 */
	zdb_status_t zdb_doc_query(zdb_t *db, const zdb_doc_query_t *query,
							   zdb_doc_metadata_t *out_metadata, size_t *out_count);

	/**
	 * @brief Release strings owned by zdb_doc_query() results.
	 *
	 * @param metadata Result array previously filled by zdb_doc_query().
	 * @param count    Number of populated entries.
	 * @retval ZDB_OK        Success.
	 * @retval ZDB_ERR_INVAL @p metadata is NULL with a non-zero @p count.
	 */
	zdb_status_t zdb_doc_metadata_free(zdb_doc_metadata_t *metadata, size_t count);

	/**
	 * @brief Serialize a document as a FlatBuffer — currently a stub.
	 *
	 * Document persistence uses ZephyrDB's own binary format, not
	 * FlatBuffers; this always returns ::ZDB_ERR_UNSUPPORTED today.
	 *
	 * @param doc          Open document handle.
	 * @param out_buf      Destination buffer.
	 * @param out_capacity Capacity of @p out_buf in bytes.
	 * @param out_len      Set to the serialized size.
	 * @retval ZDB_ERR_UNSUPPORTED Always (not implemented).
	 */
	zdb_status_t zdb_doc_export_flatbuffer(zdb_doc_t *doc, uint8_t *out_buf,
										   size_t out_capacity, size_t *out_len);

#endif /* CONFIG_ZDB_DOC */

/** @} */ /* zdb_doc */

/**
 * @addtogroup zdb_core
 * @{
 */
#if defined(CONFIG_ZDB_SHELL) && (CONFIG_ZDB_SHELL)
	/**
	 * @brief Bind an instance to the `zdb` shell command tree
	 *        (@c CONFIG_ZDB_SHELL).
	 *
	 * Registers @p db as the target of `zdb health`, `zdb stats`,
	 * `zdb kv ...`, `zdb ts ...`, and `zdb doc ...`. One instance at a
	 * time; a later call replaces the binding.
	 *
	 * @param db Initialized instance.
	 */
	void zdb_shell_register(zdb_t *db);
#endif /* CONFIG_ZDB_SHELL */

/** @} */ /* zdb_core */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYRDB_H_ */
