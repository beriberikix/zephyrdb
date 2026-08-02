# ZephyrDB Tests

All suites are standalone Zephyr test applications discovered by Twister
via their `testcase.yaml`. Each app pulls in ZephyrDB as a Zephyr module
(`EXTRA_ZEPHYR_MODULES` pointing at the repo root), so tests build through
the real Kconfig — no source-inclusion or fake `CONFIG_*` definitions.

## Suites

| Suite | Testcase ID | Backend | Covers |
|---|---|---|---|
| `unit/kv_zms` | `zephyrdb.unit.kv.zms` | ZMS on `storage_partition` (flash simulator) | KV set/get/delete round-trips, overwrite, zero-length values, size limits, namespace isolation, iterators, index persistence across re-init (namespace separation, deletes, overwrites), string helpers (round-trip, empty, truncation termination, unterminated blobs) |
| `unit/kv_nvs` | `zephyrdb.unit.kv.nvs` | NVS on `storage_partition` | Same suite source as `kv_zms`, plus the deterministic 16-bit record-ID collision test |
| `unit/kv_defaults` | `zephyrdb.unit.kv.defaults` | ZMS | Defaults table: first-boot seeding, modified values surviving re-init, new entries added on upgrade, deleted keys re-seeded, standalone apply, invalid-entry reporting; namespace reset (defaults restored, other namespaces untouched, empty namespace, keys from a previous boot) |
| `unit/kv_events` | `zephyrdb.unit.kv.events` | ZMS | Event listener dispatch: success statuses, multi-listener, null-notify slots, no-event-on-failure paths |
| `unit/ts_basic` | `zephyrdb.unit.ts_basic` | LittleFS (`boards/native_sim.overlay`) | Append (single + batch API), flush sync, aggregates with exact values, time windows, cursors (file + unflushed RAM), recovery truncation, stats, cursor reset rewind, descending traversal (reverse order, unflushed samples, windows, empty stream), consumed watermark (round-trip, persistence across re-init, corruption reads as unset, resumed drain) |
| `unit/ts_agg` | `zephyrdb.unit.ts_agg` | LittleFS | Aggregate queries with `CONFIG_ZDB_TS_MAX_AGG_POINTS=8`: uncapped COUNT (fast path and windowed scan), unflushed samples counted, empty-window semantics per aggregate, truncation reporting |
| `unit/ts_multistream` | `zephyrdb.unit.ts_multistream` | LittleFS | Four concurrent streams: interleaved appends stay separate, one flush covers every stream, slot exhaustion reports BUSY, close releases and flushes, shared handles, independent cursors, per-stream watermarks |
| `unit/ts_rollover` | `zephyrdb.unit.ts_rollover` | LittleFS | Bounded streams with 10-record segments: stays bounded and keeps the newest samples, cursors span segments in both directions, COUNT agrees with a walk, segment window survives restart, single-file streams adopted, watermark outlives discarded data |
| `unit/doc_basic` | `zephyrdb.unit.doc_basic` | LittleFS | Field CRUD for all implemented types, save/open persistence, delete, query filters (count-only, materialized, AND, limit), path-traversal rejection, header- and payload-CRC corruption, truncated files, staged-save recovery, v1 format compatibility |
| `integration/workflows` | `zephyrdb.integration.workflows` | ZMS + LittleFS | KV/TS/DOC on one instance, single-stream semantics, independent cursors, health, stats export/validate |
| `samples/verify` | `zephyrdb.samples.verify` | ZMS + LittleFS | The critical path of each sample (kv_basic, ts_basic, doc_basic) |
| `hardware/persistence_nrf52840dk` | `zephyrdb.hardware.persistence` | NVS + LittleFS on real flash | KV close/reopen persistence, TS append-log growth, recovery, multi-field DOC persistence. `build_only` in CI. |

`unit/kv_common/` holds the KV suite source shared by the ZMS and NVS apps.

Backend fixtures:

- **ZMS/NVS**: mounted on `storage_partition` (native_sim flash simulator),
  mirroring `samples/shell_basic`.
- **LittleFS**: `boards/native_sim.overlay` adds a 256 KB partition at
  0x100000 with a `zephyr,fstab,littlefs` automount at `/lfs`.
- On nrf52840dk the hardware suite reuses the (MCUboot-less) `slot1_partition`
  for LittleFS and `storage_partition` for NVS.

## Running locally

From a west workspace whose `zephyr` matches this repo's `west.yml` pin,
with the matching Zephyr SDK installed:

```bash
# Everything CI runs on native_sim:
python3 $ZEPHYR_BASE/scripts/twister -T tests -p native_sim -v --inline-logs

# One suite:
python3 $ZEPHYR_BASE/scripts/twister -T tests/unit -p native_sim \
        -s zephyrdb.unit.kv.nvs -v --inline-logs

# Hardware build gate:
python3 $ZEPHYR_BASE/scripts/twister -T tests/hardware -p nrf52840dk/nrf52840 \
        -v --inline-logs

# Execute the hardware suite on a connected board:
python3 $ZEPHYR_BASE/scripts/twister -T tests/hardware -p nrf52840dk/nrf52840 \
        --device-testing --device-serial /dev/ttyACM0
```

Note: native_sim only builds on Linux. On macOS/Windows use a Linux
container or VM for the native_sim suites; the hardware suite cross-compiles
anywhere the Zephyr SDK runs.

## Known coverage gaps

- The FCB TS backend has no dedicated suite (aggregation and recovery are
  unsupported/no-op there; see `docs/api.md`).
- FlatBuffers export tests are guarded by `CONFIG_ZDB_FLATBUFFERS` and off
  by default because CI's workspace does not fetch the flatcc-zephyr module.
- Reboot-persistence and concurrent multi-thread access are not simulated.
