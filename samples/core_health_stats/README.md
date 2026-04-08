# ZephyrDB Core Health/Stats Helper Sample

This helper sample is intended for app bring-up and diagnostics.

It demonstrates:
- core initialization/deinitialization
- health checks (`zdb_health`)
- TS stats snapshot/reset (`zdb_ts_stats_get`, `zdb_ts_stats_reset`)

Build example:

```bash
west build -p always -s samples/core_health_stats -b native_sim
west build -t run
```
