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

## Notes

- DOC samples require `flatcc-zephyr` in the same workspace.
- Filesystem-backed samples require valid storage setup for the selected board.

## Related

- [Testing Guide](testing.md)
- [Configuration](configuration.md)
- [Project README](../README.md)
