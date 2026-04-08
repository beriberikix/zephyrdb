# ZephyrDB

Embedded multi-model database for Zephyr RTOS designed for memory-constrained IoT and embedded systems.

**Current Scope:** Core + KV (NVS/ZMS-backed) + TS (LittleFS-backed) modules with durability, recovery, instrumentation, Stage 2 FlatBuffers bootstrap helpers, Stage 2.5 multi-stream support, and Stage 3 document model foundation.

## Features
- **Stage 2.5 multi-stream**: Optional concurrent stream management with unified flush and cross-stream aggregation
- **Stage 3 document model**: Semi-structured data support with flexible schemas, variable-length fields, and document CRUD APIs

- **Zero-malloc design**: Static slab-based allocation (no heap fragmentation)
- **Multiple data models**: KV (key-value via NVS or ZMS), TS (time-series with append-log via LittleFS)
- **Durability & recovery**: Versioned records with CRC32 validation; automatic corruption detection and recovery
- **Concurrency**: Reader-writer locks for safe multi-threaded access
- **Observability**: Full stats instrumentation (6 TS-specific counters) with compact telemetry export format
- **Cooperative scheduling**: Configurable yield points during scans to maintain real-time responsiveness
- **Kconfig-driven**: Comprehensive configuration for module boundaries, buffer sizing, slab dimensions, and policy
- **Stage 2 bootstrap**: Optional FlatBuffers export helper for TS samples via FlatCC runtime
- **Stage 2.5 multi-stream**: Optional concurrent stream management with unified flush and cross-stream aggregation

## Quick Start

### 1. Add to `west.yml`

```yaml
manifest:
  projects:
    - name: zephyrdb
      url: https://github.com/beriberikix/zephyrdb
      path: modules/lib/zephyrdb
      revision: main
```

### 2. Enable in `prj.conf`

```
# Core module (required)
CONFIG_ZEPHYRDB=y

# KV module (optional)
CONFIG_ZDB_KV=y
CONFIG_ZDB_KV_BACKEND_NVS=y
# or:
# CONFIG_ZDB_KV_BACKEND_ZMS=y

# TS module (optional)
CONFIG_ZDB_TS=y

# Diagnostics (optional)
CONFIG_ZDB_STATS=y
CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN=y

# Stage 2 bootstrap (optional)
CONFIG_FLATCC=y
CONFIG_ZDB_FLATBUFFERS=y
```

### 3. Initialize in your app

```c
#include <zephyrdb.h>

/* Define static slabs (zero-malloc policy) */
ZDB_DEFINE_CORE_SLAB(g_zdb_core_slab);
ZDB_DEFINE_CURSOR_SLAB(g_zdb_cursor_slab);
ZDB_DEFINE_KV_IO_SLAB(g_zdb_kv_io_slab);
ZDB_DEFINE_TS_INGEST_SLAB(g_zdb_ts_ingest_slab);

/* Create DB instance */
static zdb_t db;

static const zdb_cfg_t cfg = {
    .partition_ref = NULL,  /* Set to mounted struct nvs_fs* or struct zms_fs* for KV */
    .lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
    .kv_namespace = "app",
    .work_q = &k_sys_work_q,
    .scan_yield_every_n = CONFIG_ZDB_SCAN_YIELD_EVERY_N,
};

/* Initialize DB */
db.core_slab = &g_zdb_core_slab;
db.cursor_slab = &g_zdb_cursor_slab;
db.kv_io_slab = &g_zdb_kv_io_slab;
db.ts_ingest_slab = &g_zdb_ts_ingest_slab;
zdb_init(&db, &cfg);
```

### 4. Time-series example

```c
/* Open stream */
zdb_ts_t stream;
zdb_ts_open(&db, "metrics", &stream);

/* Append samples */
zdb_ts_sample_i64_t sample = {
    .ts_ms = k_uptime_get(),
    .value = sensor_reading(),
};
zdb_ts_append_i64(&stream, &sample);

/* Flush to disk */
zdb_ts_flush_sync(&stream, K_SECONDS(5));

/* Query aggregates */
zdb_ts_agg_result_t result;
zdb_ts_window_t window = {
    .from_ts_ms = 1000,
    .to_ts_ms = 5000,
};
zdb_ts_query_aggregate(&stream, window, ZDB_TS_AGG_AVG, &result);
printf("Average: %.2f over %u points\n", result.value, result.points);

/* Close stream */
zdb_ts_close(&stream);
```

### 5. Stats & telemetry

```c
/* Get TS-specific stats */
zdb_ts_stats_t ts_stats;
zdb_ts_stats_get(&db, &ts_stats);
printf("Recovery runs: %u\n", ts_stats.recover_runs);
printf("Corrupt records: %u\n", ts_stats.corrupt_records);

/* Export for transport/logging */
zdb_ts_stats_export_t export;
zdb_ts_stats_export(&db, &export);
if (zdb_ts_stats_export_validate(&export) == ZDB_OK) {
    /* Safe to send to telemetry service */
    transmit_telemetry(&export, sizeof(export));
}

/* Reset counters */
zdb_ts_stats_reset(&db);
```

### 6. Stage 2 FlatBuffers bootstrap

When `flatcc-zephyr` is available in your west workspace and enabled, ZephyrDB
can export a TS sample as a minimal FlatBuffer payload:

```c
zdb_ts_sample_i64_t sample = {
    .ts_ms = k_uptime_get(),
    .value = 123,
};

/* Size query pass */
size_t fb_size = 0;
zdb_status_t rc = zdb_ts_sample_i64_export_flatbuffer(&sample, NULL, 0, &fb_size);

uint8_t out[64];
if (rc == ZDB_OK && fb_size > 0 && fb_size <= sizeof(out)) {
    size_t out_len = 0;
    rc = zdb_ts_sample_i64_export_flatbuffer(&sample, out, sizeof(out), &out_len);
    if (rc == ZDB_OK) {
        /* out[0..out_len-1] now contains FlatBuffer bytes */
    }
}
```

Notes:
- If FlatBuffers support is not enabled, the API returns `ZDB_ERR_UNSUPPORTED`.
- The exported FlatBuffer root is a struct with two fields: `ts_ms` and `value`.

## Storage Isolation

- KV backends (NVS/ZMS): use a dedicated partition for ZephyrDB KV and do not share with the app's Settings backend partition.
- TS stream files are namespaced under `<mount>/<CONFIG_ZDB_TS_DIRNAME>/` (default `<mount>/zdb/`).
- Document files are namespaced under `<mount>/zdb_docs/`.
- Sharing a LittleFS mount point is supported, but storage space and wear are shared with user files in that partition.

## Architecture

### Memory Model

All long-lived allocations use static Zephyr `k_mem_slab`:
- **Core slab**: Internal context structures
- **Cursor slab**: Query/iteration cursors
- **KV IO slab**: Reserved for KV backend (unused in first-pass)
- **TS ingest slab**: Staged RAM buffer for time-series records

Configure slab dimensions via Kconfig (e.g., `CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_COUNT`).

### Concurrency

Reader-writer locks protect all shared state:
- **Read locks**: Stats retrieval, queries, iteration
- **Write locks**: Append operations, flushes, mutations

### Durability

Records and stream headers use versioned binary format with CRC32:
- **Magic number**: Detects format corruption or misalignment
- **Version field**: Enables forward-compatible evolution
- **CRC32**: Catches transmission/storage bit errors

Recovery scans to first decode failure and truncates trailing corrupt records.

### Data Models

#### KV (Key-Value)
- Backend: Zephyr NVS or Zephyr ZMS
- Use case: Configuration, calibration, firmware metadata
- API: `zdb_kv_set()`, `zdb_kv_get()`, `zdb_kv_delete()`
- Compatibility note: `zdb_kv_key_to_id()` now returns `uint32_t` (was `uint16_t`) to support ZMS IDs.

#### TS (Time-Series)
- Backend: LittleFS (file-based append-log)
- Stream files: `<mount>/<CONFIG_ZDB_TS_DIRNAME>/<stream>.zts` (default directory `zdb`)
- Staging: RAM buffer (configurable size)
- Use case: Sensor telemetry, event logs, metrics
- API: `zdb_ts_append_i64()`, `zdb_ts_query_aggregate()`, cursor iteration via `zdb_cursor_next()`

## Configuration

See `Kconfig.zephyrdb` for all 30+ options:

| Option | Default | Purpose |
|--------|---------|---------|
| `CONFIG_ZEPHYRDB` | n | Enable core module |
| `CONFIG_ZDB_KV` | y | Enable KV sub-module |
| `CONFIG_ZDB_TS` | y | Enable TS sub-module |
| `CONFIG_ZDB_STATS` | n | Enable instrumentation counters |
| `CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN` | y | Auto-recover on stream open |
| `CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES` | 4096 | Max safe truncation per recovery |
| `CONFIG_ZDB_TS_INGEST_BUFFER_BYTES` | 1024 | RAM staging buffer size |
| `CONFIG_ZDB_LFS_MOUNT_POINT` | "/lfs" | Filesystem mount for TS |
| `CONFIG_ZDB_SCAN_YIELD_EVERY_N` | 100 | Yield during scans every N records |
| `CONFIG_ZDB_FLATBUFFERS` | n | Enable Stage 2 FlatBuffers helper APIs |
| `CONFIG_ZDB_TS_MULTISTREAM` | n | Enable Stage 2.5 multi-stream support |
| `CONFIG_ZDB_TS_MAX_CONCURRENT_STREAMS` | 8 | Max simultaneous open streams (multistream mode) |
| `CONFIG_ZDB_TS_MAX_DISCOVERABLE_STREAMS` | 16 | Max streams during enumeration/discovery |

## API Reference

### Core

- `zdb_init(db, cfg)` - Initialize database
- `zdb_deinit(db)` - Shutdown, free slabs
- `zdb_health(db)` - Get health status
- `zdb_stats_get(db, out)` - Get full instrumentation snapshot
- `zdb_stats_reset(db)` - Clear all counters

### Time-Series

- `zdb_ts_open(db, stream_name, ts)` - Open/create stream
- `zdb_ts_close(ts)` - Close stream
- `zdb_ts_append_i64(ts, sample)` - Append single sample
- `zdb_ts_append_batch_i64(ts, samples, count)` - Append batch
- `zdb_ts_flush_async(ts)` - Schedule background flush
- `zdb_ts_flush_sync(ts, timeout)` - Wait for flush completion
- `zdb_ts_query_aggregate(ts, window, agg, out)` - Query min/max/avg/sum/count
- `zdb_ts_recover_stream(ts, out_bytes)` - Manual recovery (auto-run on open if enabled)
- `zdb_ts_stats_get(db, out)` - Get TS-specific stats
- `zdb_ts_stats_reset(db)` - Reset TS counters
- `zdb_ts_stats_export(db, out)` - Export stats in compact format with CRC
- `zdb_ts_stats_export_validate(export)` - Validate exported stats integrity
- `zdb_ts_sample_i64_export_flatbuffer(sample, out, cap, out_len)` - Export TS sample as FlatBuffer bytes

### Multi-Stream Management (Stage 2.5, optional)

- `zdb_ts_multistream_init(db, names, count, manager)` - Initialize multi-stream manager
- `zdb_ts_multistream_deinit(manager)` - Shutdown multi-stream manager
- `zdb_ts_multistream_count(manager)` - Get active stream count
- `zdb_ts_multistream_get_stream(manager, idx)` - Get stream by index
- `zdb_ts_multistream_find_stream(manager, name)` - Find stream by name
- `zdb_ts_multistream_flush_sync(manager, timeout)` - Coordinate flush across all streams
- `zdb_ts_multistream_query_aggregate(manager, window, agg, out)` - Aggregate query across all streams
- `zdb_ts_enum_streams(db, callback, ctx)` - Enumerate discovered streams on disk
- `zdb_ts_stream_info(db, name, out_info)` - Get stream metadata (size, record count, bounds)

### Cursor/Query

- `zdb_ts_cursor_open(ts, window, predicate, ctx, cursor)` - Open iterator
- `zdb_cursor_next(cursor, out_record)` - Iterate records
- `zdb_cursor_close(cursor)` - Free cursor

### Key-Value

- `zdb_kv_open(db, namespace, kv)` - Open KV store in namespace
- `zdb_kv_close(kv)` - Close KV
- `zdb_kv_set(kv, key, value, len)` - Write value
- `zdb_kv_get(kv, key, out_buf, cap, out_len)` - Read value
- `zdb_kv_delete(kv, key)` - Remove key

## Testing

### Board Selection

**Compilation Verification** (use `native_sim`):
- Goal: Verify code compiles and links
- **Boards:** `native_sim` (pure software simulation)
- Limitation: No FLASH storage; backends compile but cannot execute
- Usage:
  ```bash
  west build -b native_sim samples/kv_basic
  ```

**Backend & Storage Testing** (use `nrf52840dk`):
- Goal: Test NVS/ZMS/FCB backends with real persistent storage
- **Boards:** `nrf52840dk` (Cortex-M4 with 1 MB FLASH)
- Benefit: Real FLASH-backed storage; partition isolation testing; SD card support
- Usage:
  ```bash
  # NVS backend testing
  west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
  
  # ZMS backend testing
  west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_zms.conf
  ```

**Why native_sim can't test storage backends**: native_sim is a pure software simulator without FLASH hardware. NVS/ZMS backends require persistent storage to function. Tests will compile but data writes are silent no-ops (data not persisted). Use `nrf52840dk` for real storage testing.

**See [TESTING.md](TESTING.md)** for comprehensive board compatibility matrix, CI/CD strategies, and partition isolation setup.

### Sample Suite (Per Data Model + Helpers)

Dedicated samples are now provided for each model plus helper scenarios:

- `samples/kv_basic` - KV open/set/get/delete flow (accepts `prj_nrf52840dk.conf` and `prj_zms.conf` overlays)
- `samples/ts_basic` - TS open/append/flush/aggregate flow (accepts `prj_nrf52840dk.conf` overlay)
- `samples/doc_basic` - Document create/set/save/export flow (accepts `prj_nrf52840dk.conf` overlay)
- `samples/core_health_stats` - Core health + stats snapshot/reset helper
- `samples/doc_query_filters` - Document query filter construction helper

Build any sample with:

```bash
# Compilation check (native_sim)
west build -b native_sim samples/<sample_name>

# Storage backend testing (nrf52840dk)
west build -b nrf52840dk samples/<sample_name> -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
```

See `samples/README.md` and per-sample README files for board/storage notes.

### native_sim harness (Stages 1-2.5)

Run the harness on native_sim to validate API/ABI and build integration:

```bash
west build -b native_sim samples/native_sim_harness
west build -t run
```

Expected output includes:

```text
PASS: native_sim harness exported <N> bytes
```

This harness validates:
- ZephyrDB init/deinit lifecycle (Core)
- Single-stream and optional multi-stream management (TS)
- FlatBuffers size-query/export helper (`zdb_ts_sample_i64_export_flatbuffer`) [Stage 2]
- Stage 2.5 multi-stream APIs when `CONFIG_ZDB_TS_MULTISTREAM=y`
- Build integration across all modules

⚠️ **Note:** native_sim harness does **not** test persistent storage (NVS/ZMS/LittleFS). Use `nrf52840dk` samples for backend validation.

## Build System

Integrates with Zephyr's build system via:
- **CMakeLists.txt**: Declares library and sources (conditional on `CONFIG_ZEPHYRDB`)
- **Kconfig/Kconfig.zephyrdb**: Hierarchical options menu
- **module.yml**: West module manifest (for discovery)

## Limitations (First-Pass)

- **In-memory aggregation**: Aggregates limited by `CONFIG_ZDB_TS_MAX_AGG_POINTS`
- **Append-log only**: No seeking or in-place updates (intentional for durability)
- **No compression**: Records stored as-is (future enhancement)
- **FlatBuffers deferred**: Stage 2 work (Document model, variable-length schemas)

## Stage 2.5: Multi-Stream Support (optional)

When enabled with `CONFIG_ZDB_TS_MULTISTREAM=y`, allows managing multiple independent
time-series streams simultaneously:

```c
/* Initialize manager with multiple streams */
const char *stream_names[] = { "temperature", "humidity", "pressure" };
zdb_ts_multistream_t manager;
zdb_ts_multistream_init(&db, stream_names, 3, &manager);

/* Append to individual streams */
zdb_ts_t *temp_stream = zdb_ts_multistream_find_stream(&manager, "temperature");
zdb_ts_append_i64(temp_stream, &sample);

/* Flush all streams at once */
zdb_ts_multistream_flush_sync(&manager, K_SECONDS(5));

/* Global aggregate across all streams */
zdb_ts_agg_result_t global_avg;
zdb_ts_window_t window = { .from_ts_ms = 1000, .to_ts_ms = 5000 };
zdb_ts_multistream_query_aggregate(&manager, window, ZDB_TS_AGG_AVG, &global_avg);

/* Enumerate all streams on disk */
bool enum_callback(const zdb_ts_stream_info_t *info, void *ctx) {
    printf("Stream: %s, records: %u\n", info->stream_name, info->record_count);
    return true; /* continue */
}
zdb_ts_enum_streams(&db, enum_callback, NULL);

/* Cleanup */
zdb_ts_multistream_deinit(&manager);
```

Key features:
- **Concurrent streams**: Open up to `CONFIG_ZDB_TS_MAX_CONCURRENT_STREAMS` streams
- **Unified operations**: Flush and aggregate across all open streams through a single multistream API
- **Stream discovery**: Enumerate all `.zts` files and get metadata (size, record count, timestamp bounds)
- **Backward compatible**: Single-stream API remains unchanged; multistream is opt-in

## Future Work (Stage 3+)

- **Stage 3**: FlatBuffers for flexible schema evolution + document model (semi-structured data)
- **Compression**: Stream compaction (downsampling, aggregation of historical data)
- **Security**: Encryption at rest, per-stream access control
- **Replication**: Sync protocols for distributed edge deployments
- **Performance**: Hardware accelerators for aggregation, parallel scan threading

## License

See LICENSE file.

## Contributing

Contributions welcome! Please:
1. Fork repository
2. Create feature branch (`git checkout -b feature/my-feature`)
3. Commit changes with clear messages
4. Push to branch
5. Open pull request with description


## Stage 3: Document Model (Foundation Complete)

When enabled with `CONFIG_ZDB_DOC=y` (depends on `CONFIG_ZDB_FLATBUFFERS=y`), enables semi-structured data support with variable-length fields and dynamic schemas.

### Configuration Options
| Option | Default | Purpose |
|--------|---------|---------|
| `CONFIG_ZDB_DOC` | n | Enable Stage 3 document model |
| `CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN` | 32 | Max field name length |
| `CONFIG_ZDB_DOC_MAX_STRING_LEN` | 256 | Max string field size |
| `CONFIG_ZDB_DOC_MAX_FIELD_COUNT` | 32 | Max fields per document |
| `CONFIG_ZDB_DOC_MAX_NESTED_DEPTH` | 4 | Max object nesting levels |

### API Overview

**Lifecycle:**
- `zdb_doc_create(db, collection, id, out_doc)` - Create new document
- `zdb_doc_open(db, collection, id, out_doc)` - Load existing document
- `zdb_doc_save(doc)` - Persist to storage
- `zdb_doc_delete(db, collection, id)` - Remove document
- `zdb_doc_close(doc)` - Cleanup and free resources

**Fields (8 types supported):**
```
NULL, INT64, DOUBLE, STRING, BOOL, BYTES, OBJECT (nested), ARRAY
```

**Field Operations:**
- Setters: `zdb_doc_field_set_i64()`, `set_f64()`, `set_string()`, `set_bool()`, `set_bytes()`
- Getters: `zdb_doc_field_get_*()` with type validation
- Query: `zdb_doc_query()` with filters, time windows, limits
- Export: `zdb_doc_export_flatbuffer()` for schema-driven serialization

### Example Usage
```c
/* Create document */
zdb_doc_t doc;
zdb_doc_create(&db, "users", "user-42", &doc);

/* Set heterogeneous fields */
zdb_doc_field_set_string(&doc, "name", "Alice");
zdb_doc_field_set_i64(&doc, "age", 30);
zdb_doc_field_set_f64(&doc, "score", 95.5);
zdb_doc_field_set_bool(&doc, "active", true);

/* Get with type safety */
int64_t age;
zdb_doc_field_get_i64(&doc, "age", &age);

/* Persist */
zdb_doc_save(&doc);

/* Query */
zdb_doc_query_t query = {
    .filters = NULL,
    .filter_count = 0,
    .limit = 100,
};
size_t count = 0;
zdb_doc_query(&db, &query, NULL, &count);

/* Cleanup */
zdb_doc_close(&doc);
```

**Stage 3 Current Status:**
✅ Foundation APIs (CRUD, field operations)
✅ Type safety with 8 field types
✅ Dynamic field allocation
⏳ LittleFS persistence (coming)
⏳ Schema versioning (coming)
⏳ Cross-document queries (coming)

