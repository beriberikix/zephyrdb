# ZephyrDB TS Basic Sample

This sample demonstrates time-series operations:
- `zdb_ts_open`
- `zdb_ts_append_i64`
- `zdb_ts_flush_sync`
- `zdb_ts_query_aggregate`

Build example:

```bash
west build -p always -s samples/ts_basic -b <board_with_filesystem>
west build -t run
```

Backend overlays:

```bash
# Default LittleFS backend
west build -p always -s samples/ts_basic -b native_sim

# FCB backend (flash map) - requires flash-capable board
west build -p always -s samples/ts_basic -b nrf52840dk/nrf52840 -- -DOVERLAY_CONFIG=prj_fcb.conf

# SD card (LittleFS BLK mode)
west build -p always -s samples/ts_basic -b nrf52840dk/nrf52840 -- -DOVERLAY_CONFIG=prj_nrf52840dk_sdcard.conf
```

Notes:
- LittleFS-based backends require a filesystem mounted at `CONFIG_ZDB_LFS_MOUNT_POINT`.
- FCB backend uses flash map storage and does not require a filesystem mount point.
- If the selected backend storage is not configured, the sample prints a guided message and exits.
- FCB backend limitation: aggregate query returns `ZDB_ERR_UNSUPPORTED`.
