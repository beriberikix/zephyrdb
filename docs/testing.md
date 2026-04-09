# Testing Guide

ZephyrDB supports multiple backend/storage combinations, so test coverage depends on board capabilities.

## Board Selection

Use the board based on what you want to validate.

| Goal | Recommended Board | Notes |
|---|---|---|
| Fast compile verification | `native_sim` | Validates build integration and API usage patterns |
| Backend behavior and persistence | `nrf52840dk` | Validates NVS/ZMS/FCB/LittleFS behavior on flash-capable target |
| Final application validation | Production hardware | Validates board-specific runtime behavior |

## Compatibility Matrix

| Capability | native_sim | nrf52840dk |
|---|---|---|
| Build and link validation | Yes | Yes |
| NVS backend runtime behavior | Limited | Yes |
| ZMS backend runtime behavior | Limited | Yes |
| FCB backend runtime behavior | Limited | Yes |
| LittleFS-backed TS or DOC validation | Limited | Yes |
| SD card overlays | No | Yes |

## Why native_sim is limited for storage

`native_sim` is useful for compile-time and API checks but does not represent real flash persistence for backend validation.

## Build Examples

### Compile check

```bash
west build -b native_sim samples/kv_basic
west build -b native_sim samples/ts_basic
west build -b native_sim samples/doc_basic
```

Run a loop over all samples:

```bash
for sample in samples/*/; do
	west build -b native_sim "$sample" || exit 1
done
```

### Backend validation

```bash
west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_zms.conf
west build -b nrf52840dk/nrf52840 samples/ts_basic -- -DOVERLAY_CONFIG=prj_fcb.conf
west build -b nrf52840dk/nrf52840 samples/ts_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk_sdcard.conf
```

## Scenario-Based Guidance

### Verify compilation only

Use `native_sim` for fastest feedback.

### Verify KV persistence behavior

Use a flash-capable board and backend overlay (for example, `prj_nrf52840dk.conf` or `prj_zms.conf`).

### Verify TS aggregation and flush behavior

Use `nrf52840dk` with a storage backend configured in the sample overlay.

### Verify DOC workflows

Use a filesystem-capable target and ensure `flatcc-zephyr` is present in workspace.

## Recommended Workflow

1. Run compile checks on `native_sim` for all changed samples.
2. Run backend-specific tests on a flash-capable board for storage changes.
3. Run target hardware validation before release.

## Partition Isolation

When using Zephyr Settings and ZephyrDB together, allocate dedicated partitions for:

- Settings storage
- ZephyrDB KV storage
- ZephyrDB filesystem-backed TS or DOC storage

This prevents backend overlap and cross-subsystem data conflicts.

## Troubleshooting

### `CONFIG_NVS` or flash-related build errors

Switch to a flash-capable board or adjust backend options in your overlay.

### Build succeeds but persistence behavior is missing

Re-test on a flash-capable board with proper storage configuration.

### Backend reports unsupported operation

Check backend capability. Some TS operations are backend-specific (for example, aggregate query behavior).

### Serial output issues on physical boards

Verify the serial device and baud rate:

```bash
ls /dev/ttyACM* /dev/ttyUSB*
picocom --baud 115200 /dev/ttyACM0
```

## Related

- [Samples Guide](samples.md)
- [Configuration](configuration.md)
- [Project README](../README.md)
