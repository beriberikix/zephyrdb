# Architecture

## Design Goals

- No heap allocation in long-lived paths
- Predictable behavior on constrained embedded targets
- Durable storage with recovery support
- Thread-safe reads and writes

## Memory Model

ZephyrDB relies on static `k_mem_slab` pools for internal objects:

- Core slab
- Cursor slab
- KV IO slab
- TS ingest slab

The `ZDB_DEFINE_STATIC` helper wires these slabs into a `zdb_t` instance.

## Concurrency

Shared state is synchronized with Zephyr mutexes and model-specific locking paths so reads and writes can coexist safely.

## Durability and Recovery

Time-series storage uses a versioned binary format with integrity checks.

- Corruption is detected during recovery scans
- Invalid trailing records are truncated safely
- Recovery stats are exported via TS stats APIs

## Storage Isolation

- KV storage is provided through `cfg.kv_backend_fs`
- TS files are written under `<mount>/<CONFIG_ZDB_TS_DIRNAME>/`
- DOC files are written under `<mount>/zdb_docs/`

Use separate flash partitions when isolating ZephyrDB from application settings.
