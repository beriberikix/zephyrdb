# ZephyrDB Samples

This directory provides a practical sample suite for each data model plus helper-oriented reference apps.

## Data Model Samples

- `samples/kv_basic`
  - KV lifecycle: open/set/get/delete patterns
  - Best for NVS-backed integration bring-up

- `samples/ts_basic`
  - Time-series stream lifecycle, append, flush, aggregate query
  - Best for filesystem-backed metrics streams

- `samples/doc_basic`
  - Document create/set/save/export flow
  - Best for Stage 3 document model onboarding

## Helper Samples

- `samples/core_health_stats`
  - Core init/deinit, health checks, stats snapshot/reset
  - Best for sanity checks and telemetry hooks

- `samples/doc_query_filters`
  - Filter construction and metadata collection for document queries
  - Best for user-facing search/query API patterns

## Build Pattern

For any sample:

```bash
west build -p always -s samples/<sample_name> -b <board>
west build -t run
```

## Notes

- DOC samples require `flatcc-zephyr` as sibling repository.
- Storage-backed samples (KV/TS/DOC) require board/storage configuration (NVS/LittleFS) appropriate for your target.
