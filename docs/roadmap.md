# Roadmap

Unimplemented enhancements under consideration. Items are removed as they ship.

## Candidate Enhancements

### KV Enhancements

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
