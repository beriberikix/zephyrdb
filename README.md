# ZephyrDB

Embedded multi-model database for Zephyr RTOS designed for memory-constrained IoT and embedded systems.

**Current Scope:** Core + KV (NVS-backed) + TS (LittleFS-backed) modules with durability, recovery, instrumentation, and Stage 2 FlatBuffers bootstrap helpers.

## Features

- **Zero-malloc design**: Static slab-based allocation (no heap fragmentation)
- **Multiple data models**: KV (key-value via NVS), TS (time-series with append-log via LittleFS)
- **Durability & recovery**: Versioned records with CRC32 validation; automatic corruption detection and recovery
- **Concurrency**: Reader-writer locks for safe multi-threaded access
- **Observability**: Full stats instrumentation (6 TS-specific counters) with compact telemetry export format
- **Cooperative scheduling**: Configurable yield points during scans to maintain real-time responsiveness
- **Kconfig-driven**: Comprehensive configuration for module boundaries, buffer sizing, slab dimensions, and policy
- **Stage 2 bootstrap**: Optional FlatBuffers export helper for TS samples via FlatCC runtime

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
    .partition_ref = NULL,  /* Set to struct nvs_fs* for KV */
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
- Backend: Zephyr NVS (flash-based)
- Use case: Configuration, calibration, firmware metadata
- API: `zdb_kv_set()`, `zdb_kv_get()`, `zdb_kv_delete()`

#### TS (Time-Series)
- Backend: LittleFS (file-based append-log)
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

Use the runnable sample app in `samples/native_sim_harness`:

```bash
west build -p always -s samples/native_sim_harness -b native_sim
west build -t run
```

### native_sim harness (Stage 2)

Run the Stage 2 bootstrap harness on native_sim:

```bash
west build -p always -s samples/native_sim_harness -b native_sim
west build -t run
```

Expected output includes:

```text
PASS: native_sim harness exported <N> bytes
```

This harness validates:
- ZephyrDB init/deinit lifecycle
- FlatBuffers size-query/export helper (`zdb_ts_sample_i64_export_flatbuffer`)
- Stage 2 dependency wiring with flatcc-zephyr (`flatccrt` linkage)

## Build System

Integrates with Zephyr's build system via:
- **CMakeLists.txt**: Declares library and sources (conditional on `CONFIG_ZEPHYRDB`)
- **Kconfig/Kconfig.zephyrdb**: Hierarchical options menu
- **module.yml**: West module manifest (for discovery)

## Limitations (First-Pass)

- **Single active stream**: Only one TS stream open at a time (no stream multiplexing yet)
- **In-memory aggregation**: Aggregates limited by `CONFIG_ZDB_TS_MAX_AGG_POINTS`
- **Append-log only**: No seeking or in-place updates (intentional for durability)
- **No compression**: Records stored as-is (future enhancement)
- **FlatBuffers deferred**: Stage 2 work (Document model, variable-length schemas)

## Future Work (Stage 2+)

- Multi-stream support (multiple active TS streams)
- FlatBuffers for flexible schema evolution
- Document model (semi-structured data)
- Stream compaction (downsampling, aggregation)
- Encryption at rest
- Replication/sync protocols

## License

See LICENSE file.

## Contributing

Contributions welcome! Please:
1. Fork repository
2. Create feature branch (`git checkout -b feature/my-feature`)
3. Commit changes with clear messages
4. Push to branch
5. Open pull request with description
