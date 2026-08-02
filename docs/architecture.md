# Architecture

## Design Goals

- Bounded, statically sized allocation on the core and time-series paths
- Predictable behavior on constrained embedded targets
- Durable storage with recovery support
- Thread-safe reads and writes

## Memory Model

ZephyrDB relies on static `k_mem_slab` pools for internal objects:

- Core slab — time-series core context
- Cursor slab — cursor state
- KV IO slab — per-call key-value record buffers
- TS ingest slab — one ingest buffer per open time-series stream

The `ZDB_DEFINE_STATIC` helper wires these slabs into a `zdb_t` instance. Slab
blocks are the only allocation on the append, flush, and cursor paths.

Two areas still use the system heap and are sized by Kconfig rather than slabs:

- The key-value key index (`k_calloc` on first use), which holds up to
  `CONFIG_ZDB_KV_INDEX_MAX_ENTRIES` entries of two
  `CONFIG_ZDB_MAX_KEY_LEN`-sized names plus a record ID.
- The document model, which allocates the field array and every string/bytes
  payload individually. Applications that enable `CONFIG_ZDB_DOC` must size
  `CONFIG_HEAP_MEM_POOL_SIZE` accordingly.

## Concurrency

Shared state is synchronized with a single Zephyr mutex per instance, taken for
the duration of each operation. Read and write paths use the same mutex, so
operations on one instance serialize against each other; separate instances are
independent.

## Durability and Recovery

Time-series storage uses a versioned binary format with integrity checks.

- Corruption is detected during recovery scans
- Invalid trailing records are truncated safely
- Recovery stats are exported via TS stats APIs

Document storage is updated atomically. A save serializes into a staging file
and renames it over the live document, so an interrupted write leaves the
previous version intact rather than a half-written file. The record CRC covers
the header and every field payload, so corruption is detected on open instead
of being returned as valid-looking field values. An open that finds only a
staging file promotes it if it validates.

## Storage Isolation

- KV storage is provided through `cfg.kv_backend_fs`; one reserved record ID
  per backend holds the key index when `CONFIG_ZDB_KV_PERSIST_INDEX` is enabled
- TS files are written under `<mount>/<CONFIG_ZDB_TS_DIRNAME>/`: `<stream>.zts`,
  or `<stream>.NNNN.zts` segments when rollover bounds the stream, alongside a
  `<stream>.wmk` sidecar once a consumed watermark is stored
- DOC files are written under `<mount>/zdb_docs/` (`<id>.zdoc`, with
  `<id>.zdoc.tmp` appearing transiently during a save)

Use separate flash partitions when isolating ZephyrDB from application settings.
