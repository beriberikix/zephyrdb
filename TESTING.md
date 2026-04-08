# ZephyrDB Testing Guide

## Overview

ZephyrDB supports multiple storage backends. Testing strategy depends on which backends you need to validate:

| Aspect | native_sim | nrf52840dk | Physical Hardware |
|--------|-----------|-----------|------------------|
| **Primary Use Case** | Sanity checks, API validation, CI/CD gate | Integration testing, backend validation | Field deployment, real-world validation |
| **Hardware Simulation** | Pure software | ARM Cortex-M4 emulation | Real CPU/storage |
| **FLASH Support** | ❌ None | ✅ 1 MB | ✅ Device-specific |
| **NVS Backend** | 🟡 Compiles only | ✅ Fully functional | ✅ Production-ready |
| **ZMS Backend** | 🟡 Compiles only | ✅ Fully functional | ✅ Production-ready |
| **FCB Backend** | 🟡 Compiles only | ✅ Fully functional | ✅ Production-ready |
| **LittleFS (TS/DOC)** | 🟡 Compiles only | ✅ Fully functional | ✅ Production-ready |
| **SD Card** | Not applicable | ⚠️ With overlay | ✅ Production-ready |
| **Cost** | Free | ~$50 | Varies |
| **Setup Complexity** | Minimal | Moderate (QEMU or board) | Complex (hardware setup) |

### Legend
- ✅ Fully functional - real data persistence, backend behavior tested
- 🟡 Compiles only - code compiles and links, but cannot execute (no FLASH storage)
- ⚠️ With overlay - requires device tree overlay or configuration changes
- ❌ Not available - hardware not supported

## Understanding native_sim Limitations

**native_sim** is a pure software simulation that runs on your development machine. It does **not** emulate FLASH storage:

```
❌ No FLASH subsystem
   ├─ CONFIG_FLASH unavailable
   ├─ NVS backend compiles but has no storage medium
   ├─ ZMS backend compiles but has no storage medium
   └─ Tests pass trivially (data not persisted)

✅ Good for: API signatures, compile validation, non-storage features
❌ Bad for: Backend behavior, data persistence, wear-leveling, storage isolation
```

### Example: native_sim KV Backend Behavior

```bash
$ west build -b native_sim samples/kv_basic
# Builds successfully
# `zdb_kv_set()` calls succeed but data is not persisted
# No actual FLASH operations occur
```

## Using nrf52840dk for Real Backend Testing

**nrf52840dk** is the Nordic Semiconductor development kit for the nRF52840 Cortex-M4 MCU:

- **256 KB SRAM, 1 MB FLASH** - realistic embedded constraint
- **Real storage backends** - NVS, ZMS, FCB all functional
- **Partition isolation** - test separate partitions for different subsystems
- **QEMU compatibility** - can run in CI/CD without physical hardware
- **Real file systems** - LittleFS works on actual storage media

### Quick Start: nrf52840dk with native_sim emulation

If you don't have physical hardware, use QEMU to emulate nrf52840dk:

```bash
# Build for nrf52840dk (QEMU will emulate)
west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf

# Results: NVS backend fully tested with real storage behavior
```

### Quick Start: Physical nrf52840dk Hardware

```bash
# Build
west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf

# Flash to device
west flash

# Monitor Serial output (e.g., /dev/ttyACM0)
picocom --baud 115200 /dev/ttyACM0
```

## Testing Matrix: Which Board for What?

### Scenario 1: I want to verify my code compiles

```bash
# Use native_sim (faster, no setup)
west build -b native_sim samples/kv_basic
✅ Sufficient - you only need compilation verification
```

### Scenario 2: I need to test NVS/ZMS backend behavior

```bash
# Use nrf52840dk (real storage)
west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
✅ Required - backends won't work on native_sim
```

### Scenario 3: I need to test time-series streams with real storage

```bash
# Use nrf52840dk
west build -b nrf52840dk samples/ts_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
✅ Required - LittleFS can't mount real storage on native_sim
```

### Scenario 4: I'm testing SD card support

```bash
# Use nrf52840dk (with SD overlay) or physical hardware
west build -b nrf52840dk samples/ts_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk_sdcard.conf
✅ Required - SD card not available on native_sim
```

### Scenario 5: I'm doing final production validation

```bash
# Use actual target hardware (production board)
# This scenario is application-specific
```

## Recommended Testing Strategy

### For CI/CD

1. **Stage 1 - Compile Verification** (native_sim)
   ```bash
   west build -b native_sim samples/*/
   ```
   Time: ~10s per sample | Cost: Free | Coverage: API/ABI compatibility

2. **Stage 2 - Integration Testing** (nrf52840dk/QEMU)
   ```bash
   west build -b nrf52840dk samples/*/ -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
   ```
   Time: ~30s per sample | Cost: Free (if QEMU available) | Coverage: Backend behavior, storage isolation

3. **Stage 3 - Field Validation** (Production hardware, optional)
   - Application-specific, varies by deployment target

### For Local Development

1. **Before committing**: Verify all samples compile on native_sim
   ```bash
   for sample in samples/*/; do
       west build -b native_sim "$sample" || exit 1
   done
   ```

2. **Before making storage-related changes**: Test on nrf52840dk
   ```bash
   # With QEMU (if available)
   west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
   
   # Or with physical hardware
   west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
   west flash
   ```

## Sample Configurations

### Default Configurations (native_sim compatible)

```bash
# Compile check - works on native_sim
west build -b native_sim samples/kv_basic
```

### Board-Specific Overlays (real hardware)

**Note:** Board-specific overlays like `prj_nrf52840dk.conf` are provided for reference but contain commented-out hardware-specific options (e.g., `CONFIG_FLASH`, `CONFIG_FILE_SYSTEM_LITTLEFS`) that are not available on `native_sim`. When building for real hardware, uncomment these options in the overlay.

#### With physical hardware or QEMU emulation:

For nrf52840dk with actual hardware or when all hardware dependencies are available:

1. Uncomment the hardware options in `prj_nrf52840dk.conf`:
   ```
   CONFIG_FLASH=y
   CONFIG_NVS=y
   ```

2. Build with the overlay:
   ```bash
   west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
   ```

3. If building for QEMU emulation (hardware simulation):
   ```bash
   # QEMU will emulate the nrf52840dk with FLASH support
   west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
   ```

#### Alternative backends (physical hardware only):

```bash
# nrf52840dk with ZMS backend
west build -b nrf52840dk samples/kv_basic -- -DOVERLAY_CONFIG=prj_zms.conf

# nrf52840dk with time-series streams
west build -b nrf52840dk samples/ts_basic -- -DOVERLAY_CONFIG=prj_nrf52840dk.conf
```

## Partition Isolation Testing

To verify that Settings (Zephyr system subsystem) and ZephyrDB can coexist on the same device:

1. **Create separate partitions** in your board DTS:
   ```dts
   / {
       storage_partitions: partitions {
           compatible = "fixed-partitions";
           #address-cells = <1>;
           #size-cells = <1>;

           storage_partition: partition@0 {
               label = "nvs_storage";  // For Zephyr Settings
               reg = <0x0 0x10000>;   // 64 KB
           };

           zdb_storage_partition: partition@10000 {
               label = "zdb_storage";  // For ZephyrDB KV backend
               reg = <0x10000 0x10000>; // 64 KB
           };

           zdb_fs_partition: partition@20000 {
               label = "zdb_fs";       // For ZephyrDB TS/DOC
               reg = <0x20000 0x20000>; // 128 KB
           };
       };
   };
   ```

2. **Configure ZephyrDB to use the separate partition**:
   ```c
   // Initialize Settings subsystem (uses storage_partition)
   settings_load();

   // Initialize ZephyrDB with separate partition
   struct nvs_fs zdb_nvs;
   struct fs_mount_t zdb_fs_mount;
   
   zdb_cfg_t cfg = {
       .partition_ref = (void *)&zdb_nvs,  // Points to ZDB partition
       .lfs_mount_point = "/zdb_fs"        // Points to ZDB FS partition
   };
   ```

## Advanced: Adding New Board Support

To add support for a new board:

1. **Create a sample overlay**: `samples/kv_basic/prj_<board>.conf`
   ```
   CONFIG_ZDB_KV_BACKEND_NVS=y
   CONFIG_NVS=y
   CONFIG_FLASH=y
   ```

2. **If partition isolation needed**: Create a DTS overlay
   ```
   samples/kv_basic/<board>.overlay
   ```

3. **Test**:
   ```bash
   west build -b <board> samples/kv_basic -- -DOVERLAY_CONFIG=prj_<board>.conf
   ```

## Troubleshooting

### Build Error: "CONFIG_NVS not found"

You're trying to build NVS backend on a board without FLASH support. Solutions:

```bash
# Option 1: Use a board with FLASH
west build -b nrf52840dk samples/kv_basic

# Option 2: Disable KV backend
# In prj.conf:
CONFIG_ZDB_KV=n
```

### Build succeeds on native_sim but KV operations fail in code

**Expected behavior**. native_sim has no persistent storage. Either:

- Don't test persistence on native_sim (use nrf52840dk instead)
- Mock/stub the backend for unit tests
- Use native_sim for API/ABI validation only

### Serial output not appearing on nrf52840dk

Ensure you're monitoring the correct serial port:

```bash
# List available ports
ls /dev/ttyACM* /dev/ttyUSB*

# Monitor with correct settings
picocom --baud 115200 /dev/ttyACM0
```

## References

- [Zephyr nrf52840dk Documentation](https://docs.zephyrproject.org/latest/boards/arm/nrf52840dk_nrf52840/doc/index.html)
- [Zephyr NVS Documentation](https://docs.zephyrproject.org/latest/services/storage/nvs/nvs.html)
- [Zephyr ZMS Documentation](https://docs.zephyrproject.org/latest/services/storage/zms/index.html)
- [ZephyrDB README](README.md)
