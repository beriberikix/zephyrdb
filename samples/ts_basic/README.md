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

Notes:
- Requires a filesystem mounted at `CONFIG_ZDB_LFS_MOUNT_POINT`.
- If mount/storage is not configured, sample prints a guided message and exits.
