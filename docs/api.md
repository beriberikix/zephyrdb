# API Reference

This page summarizes the currently implemented ZephyrDB public APIs.

## Build-Time Guards

- Core APIs are available when `CONFIG_ZEPHYRDB=y`.
- KV APIs are available when `CONFIG_ZDB_KV=y`.
- TS APIs are available when `CONFIG_ZDB_TS=y`.
- Document APIs are available when `CONFIG_ZDB_DOC=y`.
- FlatBuffers export helper requires `CONFIG_ZDB_FLATBUFFERS=y` and `CONFIG_FLATCC=y`.
- Eventing APIs are available when `CONFIG_ZDB_EVENTING=y`.
- zbus adapter APIs are available when `CONFIG_ZDB_EVENTING_ZBUS=y`.

## Core

- `zdb_init(db, cfg)`
- `zdb_deinit(db)`
- `zdb_health(db)`
- `zdb_ts_stats_get(db, out)`
- `zdb_ts_stats_reset(db)`
- `zdb_ts_stats_export(db, out_export)`
- `zdb_ts_stats_export_validate(export)`

## Key-Value

- `zdb_kv_open(db, namespace_name, kv)`
- `zdb_kv_close(kv)`
- `zdb_kv_set(kv, key, value, value_len)`
- `zdb_kv_get(kv, key, out_value, out_capacity, out_len)`
- `zdb_kv_delete(kv, key)`

## Eventing

- `zdb_event_type_t`
- `zdb_kv_event_t`
- `zdb_event_listener_fn_t`
- `zdb_event_listener_t`

Configuration fields in `zdb_cfg_t` when eventing is enabled:

- `event_listeners`
- `event_listener_count`

## zbus Adapter

Header:

- `zephyrdb_eventing_zbus.h`

Symbols:

- `ZBUS_CHAN_DECLARE(zdb_kv_event_chan)`
- `zdb_eventing_zbus_publish(const zdb_kv_event_t *event)`

Channel payload type:

- `zdb_kv_event_t`

## Time-Series

- `zdb_ts_open(db, stream_name, ts)`
- `zdb_ts_close(ts)`
- `zdb_ts_append_i64(ts, sample)`
- `zdb_ts_append_batch_i64(ts, samples, sample_count)`
- `zdb_ts_flush_async(ts)`
- `zdb_ts_flush_sync(ts, timeout)`
- `zdb_ts_query_aggregate(ts, window, agg, out_result)`
- `zdb_ts_recover_stream(ts, out_truncated_bytes)`
- `zdb_ts_cursor_open(ts, window, predicate, predicate_ctx, out_cursor)`
- `zdb_cursor_next(cursor, out_record)`
- `zdb_cursor_reset(cursor)`
- `zdb_cursor_close(cursor)`

Notes:

- `zdb_ts_query_aggregate()` is backend-dependent and may return `ZDB_ERR_UNSUPPORTED` on non-aggregate-capable backends.

## FlatBuffers Export Helper

- `zdb_ts_sample_i64_export_flatbuffer(sample, out_buf, out_capacity, out_len)`

## Document Model

- `zdb_doc_create(db, collection_name, document_id, out_doc)`
- `zdb_doc_open(db, collection_name, document_id, out_doc)`
- `zdb_doc_save(doc)`
- `zdb_doc_delete(db, collection_name, document_id)`
- `zdb_doc_close(doc)`
- `zdb_doc_field_set_i64(doc, field_name, value)`
- `zdb_doc_field_set_f64(doc, field_name, value)`
- `zdb_doc_field_set_string(doc, field_name, value)`
- `zdb_doc_field_set_bool(doc, field_name, value)`
- `zdb_doc_field_set_bytes(doc, field_name, value, len)`
- `zdb_doc_field_get_i64(doc, field_name, out_value)`
- `zdb_doc_field_get_f64(doc, field_name, out_value)`
- `zdb_doc_field_get_string(doc, field_name, out_value)`
- `zdb_doc_field_get_bool(doc, field_name, out_value)`
- `zdb_doc_field_get_bytes(doc, field_name, out_value)`
- `zdb_doc_query(db, query, out_metadata, out_count)`
- `zdb_doc_metadata_free(metadata, count)`
- `zdb_doc_export_flatbuffer(doc, out_buf, out_capacity, out_len)`

## Source of Truth

For exact signatures and compile-time guards, see [../zephyrdb.h](../zephyrdb.h).
