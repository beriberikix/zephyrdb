# Roadmap

This page tracks potential future enhancements separately from implementation-focused documentation.

## Critical Issues

These are high-priority bugs identified in code analysis:

- **C1: DOC module nesting bug** — The entire `CONFIG_ZDB_DOC` implementation is accidentally nested inside the `CONFIG_ZDB_TS` pragma block. Users can configure `CONFIG_ZDB_DOC=y` with `CONFIG_ZDB_TS=n`, resulting in link-time failures. Fix: Restructure zephyrdb.c to separate TS and DOC blocks.
- **C2: DOC FlatBuffers export unimplemented** — `zdb_doc_export_flatbuffer()` is a placeholder that returns success but writes garbage data. Fix: Implement using FlatCC builder pattern or return `ZDB_ERR_UNSUPPORTED`.
- **C3: KV key collision detection** — NVS (16-bit) and ZMS (32-bit) backends use hash-based IDs with no collision detection. Collisions silently corrupt data. Fix: Store full key alongside value or implement chain resolution.
- **C4: TS flush_sync spin-wait** — `zdb_ts_flush_sync()` uses `k_yield()` busy-wait. On cooperative schedulers, this can block indefinitely. Fix: Replace with `k_sem`/`k_event` signaling.
- **C5: TS cursor file re-opening** — `zdb_cursor_next()` opens file per call instead of caching. Causes O(n) file opens. Fix: Cache file handle in cursor context.

## Candidate Enhancements

### KV Enhancements

- **KV iterator/traversal** — Add API to walk all stored KV pairs (cf. FlashDB `fdb_kv_iterate`). Enables diagnostics, export, and migration workflows.
- **KV default values with auto-init** — Accept a default KV table at init; on first boot write defaults automatically. Useful for product parameter bootstrapping.
- **KV incremental upgrade** — After firmware updates, auto-merge new default KVs into existing database without losing user-modified values. Solves a common deployment pain point.
- **KV string type convenience API** — Add dedicated string get/set alongside raw blob to reduce boilerplate for the common string case.
- **KV reset to defaults** — Factory-reset API that wipes all KVs back to the default table.

### TS Enhancements

- **TS record status lifecycle** — Add a mutable status field per record (e.g. write → read → delete) to support mark-as-processed workflows without deleting data.
- **TS time-range iteration** — Cursor-based iteration filtered by `[from, to]` timestamps, with both forward and reverse traversal.
- **TS record count query** — Lightweight metadata query to count records matching time range and/or status criteria without reading payloads.
- **TS rollover / circular buffer mode** — First-class DB-level option to automatically overwrite oldest data when storage is full (FCB backend may provide this indirectly, but it should be an explicit, backend-agnostic feature).
- Multi-stream time-series coordination APIs
- Compression and long-window compaction for TS data

### Other

- **Runtime sector/partition size control** — Allow tuning storage sector size per database instance at runtime to control max record size vs. storage efficiency tradeoffs.
- Extended document schema evolution support
- Additional security hardening for data at rest
- Synchronization patterns for distributed edge deployments

## Notes

Items on this page are planning candidates and may change.
