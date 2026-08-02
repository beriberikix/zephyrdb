# Configuration

ZephyrDB is configured through [../Kconfig.zephyrdb](../Kconfig.zephyrdb).
This page covers the complete option surface, grouped as in the Kconfig menus.

## Top Level

- `CONFIG_ZEPHYRDB`: enable ZephyrDB (requires `FLASH` or native_sim)
- `CONFIG_ZDB_CORE`: core engine (storage hooks, cursors, slab pools); default y
- `CONFIG_ZDB_KV`: KV module; default y
- `CONFIG_ZDB_TS`: time-series module; default y, requires `FILE_SYSTEM` or `FCB`

### KV backend (choice, default NVS)

- `CONFIG_ZDB_KV_BACKEND_NVS`: NVS backend (requires `CONFIG_NVS`); 16-bit record IDs
- `CONFIG_ZDB_KV_BACKEND_ZMS`: ZMS backend (requires `CONFIG_ZMS`); 32-bit record IDs

### TS backend (choice, default LittleFS)

- `CONFIG_ZDB_TS_BACKEND_LITTLEFS`: append-log files on a mounted filesystem
- `CONFIG_ZDB_TS_BACKEND_FCB`: Flash Circular Buffer (no aggregation/recovery;
  see [api.md](api.md))

## Core Boundaries

- `CONFIG_ZDB_MAX_KEY_LEN` (8–128, default 48): max namespace/key length.
  Also scales the KV RAM index: the session key index is
  `128 × (2 × (ZDB_MAX_KEY_LEN + 1) + 4)` bytes of `k_calloc` heap
  (~13 KB at the default), so budget `CONFIG_HEAP_MEM_POOL_SIZE` accordingly.
- `CONFIG_ZDB_SCAN_YIELD_EVERY_N` (default 64): cooperative yield interval
  for long scans
- `CONFIG_ZDB_CORE_SLAB_BLOCK_SIZE` / `CONFIG_ZDB_CORE_SLAB_BLOCK_COUNT`
  (defaults 128 / 16, block size 512 with the FCB TS backend): core context
  allocations. The block size must hold the time-series core context; the
  build asserts this, so a too-small value fails to compile.
- `CONFIG_ZDB_CURSOR_SLAB_BLOCK_SIZE` / `CONFIG_ZDB_CURSOR_SLAB_BLOCK_COUNT`
  (defaults 128 / 8): cursor contexts; the block count bounds concurrently
  open cursors. The block size must hold a time-series cursor context (which
  is larger on 64-bit targets) and is likewise build-asserted.

## KV Boundaries

- `CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE` (default 128): KV IO buffer size. The
  maximum stored value is `block_size − 3 − strlen(namespace) − strlen(key)`.
- `CONFIG_ZDB_KV_IO_SLAB_BLOCK_COUNT` (default 8): concurrent KV operations
- `CONFIG_ZDB_KV_INDEX_MAX_ENTRIES` (range 8–1024, default 128): keys the
  iterator can enumerate. Costs about `2 * (ZDB_MAX_KEY_LEN + 1) + 4` bytes of
  heap per entry, plus 4 bytes of storage per entry when the index is
  persisted. Keys past this bound stay readable but are not iterable.
- `CONFIG_ZDB_KV_PERSIST_INDEX` (default y): mirror the key index into a
  reserved backend record so iteration covers keys written before the current
  boot. Set to `n` for session-only iteration without the extra write on key
  creation and deletion.

## TS Boundaries

- `CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN` (default 24)
- `CONFIG_ZDB_TS_INGEST_BUFFER_BYTES` (default 1024, LittleFS): RAM staging
  before flush
- `CONFIG_ZDB_TS_MAX_AGG_POINTS` (default 4096, LittleFS): scan cap for
  MIN/MAX/AVG/SUM. Reaching it sets `result.truncated` rather than silently
  returning a partial answer. `ZDB_TS_AGG_COUNT` ignores this cap.
- `CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE` / `CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_COUNT`
  (defaults 64 / 32)
- `CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN` (default y, LittleFS): scan and
  truncate trailing corrupt records on `zdb_ts_open()`
- `CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES` (default 4096, LittleFS):
  safety cap for automatic truncation; 0 disables the cap
- `CONFIG_ZDB_TS_FCB_FLASH_AREA_ID` / `CONFIG_ZDB_TS_FCB_SECTOR_COUNT`
  (FCB backend): flash area wiring

## Storage Integration

- `CONFIG_ZDB_LFS_MOUNT_POINT` (default "/lfs"): convenience default for
  applications — pass it as `zdb_cfg_t.lfs_mount_point`. The library never
  reads this symbol directly; the path must match a filesystem the
  application mounted (or an fstab automount).
- `CONFIG_ZDB_TS_DIRNAME` (default "zdb"): TS stream directory under the
  mount point. Document files always live under `<mount>/zdb_docs/`.

## Diagnostics

- `CONFIG_ZDB_EVENTING`: local KV/TS/DOC mutation events (in-process
  listeners; best-effort, no durability guarantees). Available whenever any of
  KV, TS, or DOC is enabled; events are emitted for the enabled modules.
- `CONFIG_ZDB_EVENTING_ZBUS`: publish events to zbus channels (requires
  `CONFIG_ZDB_EVENTING` and `CONFIG_ZBUS`); best-effort, never changes
  operation return values
- `CONFIG_ZDB_STATS`: TS statistics counters (recover runs/failures,
  CRC failures, corrupt records)

## Shell

- `CONFIG_ZDB_SHELL`: `zdb` command tree (requires `CONFIG_SHELL`);
  register an instance with `zdb_shell_register()`

## Stage 2 (Experimental)

- `CONFIG_ZDB_FLATBUFFERS`: TS sample FlatBuffers export helper (requires
  `CONFIG_FLATCC` from the flatcc-zephyr module)
- `CONFIG_ZDB_DOC`: document model (requires `FILE_SYSTEM`; documents are
  stored as files on the mounted filesystem)
- `CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN` (default 32)
- `CONFIG_ZDB_DOC_MAX_STRING_LEN` (default 256)
- `CONFIG_ZDB_DOC_MAX_BYTES_LEN` (default 4096)
- `CONFIG_ZDB_DOC_MAX_FIELD_COUNT` (default 32)

## Practical Notes

- Use board overlays to provide the flash partitions / fstab mounts your
  backends need (see `tests/*/boards/native_sim.overlay` for a LittleFS
  example and `samples/shell_basic` for ZMS on `storage_partition`).
- Planned-but-unimplemented features are tracked in
  [roadmap.md](roadmap.md), not as Kconfig symbols.
