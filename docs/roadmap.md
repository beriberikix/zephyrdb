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

- Multi-stream time-series coordination APIs
- Compression and long-window compaction for TS data
- Extended document schema evolution support
- Additional security hardening for data at rest
- Synchronization patterns for distributed edge deployments

## Notes

Items on this page are planning candidates and may change.
