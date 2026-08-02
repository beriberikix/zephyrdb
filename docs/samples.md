# Samples Guide

This page centralizes sample documentation.

## Sample Catalog

Use these samples as implementation references for each ZephyrDB module.

### kv_basic

Key-value lifecycle sample demonstrating open, set, get, and delete operations.

Build:

```bash
west build -p always -s samples/kv_basic -b <your_flash_board>
west build -t run
```

Notes:

- Designed for flash-backed KV validation.
- For nrf52840dk overlays, use `prj_nrf52840dk.conf` (NVS) or `prj_zms.conf` (ZMS).

### ts_basic

Time-series sample demonstrating stream open, append, flush, and aggregate query APIs.

Build:

```bash
west build -p always -s samples/ts_basic -b <board_with_filesystem>
west build -t run
```

Common overlays:

```bash
west build -p always -s samples/ts_basic -b nrf52840dk/nrf52840 -- -DOVERLAY_CONFIG=prj_fcb.conf
west build -p always -s samples/ts_basic -b nrf52840dk/nrf52840 -- -DOVERLAY_CONFIG=prj_nrf52840dk_sdcard.conf
```

Notes:

- Filesystem-backed paths require a valid `CONFIG_ZDB_LFS_MOUNT_POINT`.
- FCB mode is intended for flash-map based storage and has backend-specific behavior.

### doc_basic

Document lifecycle sample demonstrating create, field updates, save, and FlatBuffers export.

Build:

```bash
west build -p always -s samples/doc_basic -b <board_with_filesystem>
west build -t run
```

Notes:

- Requires filesystem-backed storage.
- Requires `flatcc-zephyr` in the same workspace.

### core_health_stats

Helper sample for initialization, health checks, and stats snapshot or reset.

Build:

```bash
west build -p always -s samples/core_health_stats -b native_sim
west build -t run
```

### doc_query_filters

Helper sample for building query filters and reading metadata results.

Build:

```bash
west build -p always -s samples/doc_query_filters -b <board_with_filesystem>
west build -t run
```

Notes:

- Requires filesystem-backed storage and `flatcc-zephyr` dependency.

### shell_basic

Developer-oriented Zephyr shell sample that exposes the `zdb` command tree for common runtime operations.

Build:

```bash
west build -p always -s samples/shell_basic -b native_sim
west build -t run
```

Notes:

- Uses ZMS on `storage_partition` for KV backend on `native_sim`.
- Runs a startup KV smoke check (`set/get/delete`) before enabling interactive shell usage.
- Useful for validating command behavior such as `zdb health`, `zdb stats`, and `zdb kv ...`.

### eventing_zbus

Demonstrates KV, TS, and DOC mutation event emission published to zbus through the optional adapter.

Build:

```bash
west build -p always -s samples/eventing_zbus -b native_sim
west build -t run
```

Notes:

- Enables `CONFIG_ZDB_EVENTING` and `CONFIG_ZDB_EVENTING_ZBUS`.
- Uses ZMS on `storage_partition` for KV backend on `native_sim`.
- Prints latest zbus events for KV (`zdb_kv_event_chan`), TS (`zdb_ts_event_chan`), and DOC (`zdb_doc_event_chan`).

### doc_kv_blob

Stores a structured record as a single KV value on boards without a
filesystem: a versioned packed struct, seeded by a defaults table, read back
with v1→v2 upgrade handling, and cleared with `zdb_kv_reset_namespace()`.

Build:

```bash
west build -p always -s samples/doc_kv_blob -b native_sim
west build -t run
```

Notes:

- Uses ZMS on `storage_partition`; no filesystem required.
- Shows the interim pattern for filesystem-less boards (see issue #34): fine
  when the record's shape is known at compile time, whereas the document model
  suits ad-hoc or queryable fields.
- The reader keys off the record's leading version field rather than its
  stored length, so an unknown layout is reported instead of reinterpreted.
- Prints `doc_kv_blob: PASS` after seeding, upgrading, and factory-resetting.

### doc_cbor_nested

Stores a nested structure in a document field by encoding it with CBOR
(zcbor) and keeping the result in a `BYTES` field, alongside ordinary flat
fields.

Build:

```bash
west build -p always -s samples/doc_cbor_nested -b native_sim
west build -t run
```

Notes:

- Uses LittleFS via `boards/native_sim.overlay`; needs `CONFIG_ZCBOR=y`.
- Shows the pattern for nested values (see issue #33): encode structure into
  bytes when a field has shape of its own.
- The encoded field is opaque to `zdb_doc_query`, so anything you filter on
  should stay a flat field — the sample keeps `sensor_id` flat for that reason.
- Prints `doc_cbor_nested: PASS` after a save, reopen, and decode round-trip.

### native_sim_harness

Minimal `native_sim` harness that initializes an instance and exercises the time-series FlatBuffers export helper in both size-query and write modes.

Build:

```bash
west build -p always -s samples/native_sim_harness -b native_sim
west build -t run
```

Notes:

- Requires `flatcc-zephyr` in the same workspace (`CONFIG_ZDB_FLATBUFFERS=y`, `CONFIG_FLATCC=y`).
- Builds with `CONFIG_ZDB_KV=n`; no storage backend is mounted.
- Prints `PASS: native_sim harness exported <n> bytes` on success — useful as a smoke check that the flatcc runtime is wired into the workspace.

## Notes

- DOC samples require `flatcc-zephyr` in the same workspace.
- Filesystem-backed samples require valid storage setup for the selected board.

## Related

- [Testing Guide](testing.md)
- [Configuration](configuration.md)
- [Project README](../README.md)
