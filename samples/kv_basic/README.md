# ZephyrDB KV Basic Sample

This sample demonstrates the KV data model (`zdb_kv_open`, `zdb_kv_set`, `zdb_kv_get`) with NVS.

Build example:

```bash
west build -p always -s samples/kv_basic -b <your_flash_board>
west build -t run
```

Notes:
- Requires board/storage setup that supports NVS.
- `kv_backend_fs` is intentionally left `NULL` for portability; wire your board's NVS partition in production apps.
