# API Reference

This page summarizes the ZephyrDB public API. For exact signatures and
compile-time guards, see [../zephyrdb.h](../zephyrdb.h) — the header is the
source of truth. A browsable Doxygen reference generated from the headers
is hosted at <https://beriberikix.github.io/zephyrdb/>.

## Build-Time Guards

- Core APIs are available when `CONFIG_ZEPHYRDB=y`.
- KV APIs are available when `CONFIG_ZDB_KV=y`.
- TS APIs are available when `CONFIG_ZDB_TS=y`.
- Document APIs are available when `CONFIG_ZDB_DOC=y` (requires a mounted
  filesystem, e.g. LittleFS).
- FlatBuffers export helper requires `CONFIG_ZDB_FLATBUFFERS=y` and `CONFIG_FLATCC=y`.
- Eventing APIs are available when `CONFIG_ZDB_EVENTING=y`.
- zbus adapter APIs are available when `CONFIG_ZDB_EVENTING_ZBUS=y`.
- Shell commands are available when `CONFIG_ZDB_SHELL=y`.

## Status Codes

All APIs return `zdb_status_t`: `ZDB_OK`, `ZDB_ERR_INVAL`, `ZDB_ERR_NOMEM`,
`ZDB_ERR_NOT_FOUND`, `ZDB_ERR_IO`, `ZDB_ERR_BUSY`, `ZDB_ERR_TIMEOUT`,
`ZDB_ERR_UNSUPPORTED`, `ZDB_ERR_CORRUPT`, `ZDB_ERR_INTERNAL`,
`ZDB_ERR_COLLISION`. `zdb_status_str(status)` returns a short printable name.

## Instance Setup

- `ZDB_DEFINE_STATIC(db_name, cfg_name)` — declares the Kconfig-sized memory
  slabs (core, cursor, and the KV/TS IO slabs for enabled modules) and a
  `static zdb_t db_name` wired to them. The `cfg_name` argument is currently
  unused by the macro; pass the same `zdb_cfg_t` to `zdb_init()`.
- Lower-level slab macros: `ZDB_MEM_SLAB_DEFINE`, `ZDB_DEFINE_CORE_SLAB`,
  `ZDB_DEFINE_CURSOR_SLAB`, `ZDB_DEFINE_KV_IO_SLAB`, `ZDB_DEFINE_TS_INGEST_SLAB`.

## Core

- `zdb_init(db, cfg)` / `zdb_deinit(db)`
- `zdb_health(db)` — returns `zdb_health_t` (`OK`/`DEGRADED`/`READONLY`/`FAULT`)
- `zdb_status_str(status)` — printable status name
- `zdb_ts_stats_get(db, out)` / `zdb_ts_stats_reset(db)`
- `zdb_ts_stats_export(db, out_export)` — fills a packed, CRC-protected
  telemetry record; the CRC covers every byte except the crc field itself
- `zdb_ts_stats_export_validate(export)` — `ZDB_ERR_CORRUPT` on CRC mismatch,
  `ZDB_ERR_UNSUPPORTED` on unknown version

### Cursors (core framework, currently used by TS)

- `zdb_cursor_next(cursor, out_record)` — declared with the TS module today
- `zdb_cursor_reset(cursor)`
- `zdb_cursor_close(cursor)`

## Key-Value

- `zdb_kv_open(db, namespace_name, kv)` / `zdb_kv_close(kv)`
- `zdb_kv_set(kv, key, value, value_len)`
- `zdb_kv_get(kv, key, out_value, out_capacity, out_len)`
- `zdb_kv_set_str(kv, key, value)` / `zdb_kv_get_str(kv, key, out_str, out_capacity, out_len)`
- `zdb_kv_defaults_apply(db)` / `zdb_kv_reset_namespace(kv)`
- `zdb_kv_delete(kv, key)`
- `zdb_kv_iter_open(kv, out_iter)`
- `zdb_kv_iter_next(iter, out_key, out_key_capacity, out_key_len, out_value, out_value_capacity, out_value_len)`
- `zdb_kv_iter_close(iter)`

Storage format (v2): each record is stored as
`[0xDB tag][ns_len][key_len][namespace][key][value]` under a backend record
ID hashed from the namespace and key together, so namespaces are isolated in
storage and reads verify the full identity of the stored record.

Notes:

- `zdb_kv_set` returns `ZDB_ERR_COLLISION` when the key's backend record ID
  is already occupied by a **different** (namespace, key); the stored record
  is untouched. The remedy is choosing a different key name. With the NVS
  backend the ID space is 16-bit, so collisions are rare but reachable.
- The maximum value size is
  `CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE - 3 - strlen(namespace) - strlen(key)`;
  larger values fail with `ZDB_ERR_NOMEM`.
- `zdb_kv_get` never fails on a too-small output buffer: it copies up to
  `out_capacity` bytes and reports the full stored length in `*out_len`.
  Compare `*out_len` against your capacity to detect truncation.
- A zero-length value is valid; `value` may be NULL when `value_len` is 0.
- Setting `cfg.kv_defaults` / `cfg.kv_default_count` makes `zdb_init` seed any
  key in the table that is **absent**, leaving existing keys untouched. That
  covers first boot (everything is written) and a firmware update that adds
  entries (only the new keys are written, so values the user changed survive).
  `zdb_kv_defaults_apply` runs the same pass on demand. Every entry is
  attempted even if one fails, and the first failing status is returned — so
  `zdb_init` can now report backend errors when a defaults table is
  configured. A key the application deleted is indistinguishable from one never
  written, so it is re-seeded on the next pass.
- `zdb_kv_reset_namespace` is the factory-reset primitive: it deletes every key
  the index holds for that namespace, then re-applies the namespace's defaults,
  leaving it as a first boot would. Other namespaces are untouched and each
  deletion emits an event. It can only delete what the iterator can see, so the
  `CONFIG_ZDB_KV_INDEX_MAX_ENTRIES` bound applies, and with
  `CONFIG_ZDB_KV_PERSIST_INDEX=n` it clears only keys touched this session.
- The string helpers wrap the byte API for the common case: `zdb_kv_set_str`
  stores `strlen(value) + 1` bytes so the terminator is part of the value, and
  `zdb_kv_get_str` always terminates its output, copying at most
  `out_capacity - 1` bytes. Detect truncation with `*out_len + 1 >
  out_capacity`. `zdb_kv_get_str` also handles values written through
  `zdb_kv_set` without a terminator.
- Records written by pre-v2 builds are treated as absent (reads and deletes
  report `ZDB_ERR_NOT_FOUND`); a set reclaims the slot.
- The iterator enumerates every key the index knows about, including keys
  written before this boot. The index is held in RAM and, with
  `CONFIG_ZDB_KV_PERSIST_INDEX=y` (the default), mirrored into a reserved
  backend record so it can be rebuilt after a restart. Rebuilding reads one
  record per tracked key on first use. Set `CONFIG_ZDB_KV_PERSIST_INDEX=n` for
  session-only iteration, which skips the extra write when a key is created or
  deleted.
- The index holds at most `CONFIG_ZDB_KV_INDEX_MAX_ENTRIES` keys (default 128).
  Keys beyond that are **stored and readable but not enumerable**, so raise the
  bound if a deployment relies on iterating everything.
- A crash between the index write and the data write leaves an index entry with
  no record. Iteration skips it and the next rebuild removes it; stored data is
  never affected.

## Eventing

Listener types (each embeds a `notify` function pointer and `user_ctx`):

- `zdb_event_listener_t` for `zdb_kv_event_t` (types `ZDB_EVENT_KV_SET`/`ZDB_EVENT_KV_DELETE`)
- `zdb_ts_event_listener_t` for `zdb_ts_event_t` (append/flush/recover)
- `zdb_doc_event_listener_t` for `zdb_doc_event_t` (create/save/delete)

Register listener arrays through `zdb_cfg_t` before `zdb_init()`:

```c
static void on_kv_event(const zdb_kv_event_t *event, void *user_ctx)
{
    printk("kv %s/%s -> %s\n", event->namespace_name, event->key,
           zdb_status_str(event->status));
}

static const zdb_event_listener_t listeners[] = {
    { .notify = on_kv_event, .user_ctx = NULL },
};

static const zdb_cfg_t cfg = {
    /* ... */
    .event_listeners = listeners,
    .event_listener_count = ARRAY_SIZE(listeners),
};
```

Events fire after each operation with its real status (including failures
and `ZDB_ERR_COLLISION`); entries with a NULL `notify` are skipped.
Corresponding `ts_event_listeners`/`doc_event_listeners` fields exist when
TS/DOC are enabled.

## zbus Adapter

Header: `zephyrdb_eventing_zbus.h`

- Channels: `zdb_kv_event_chan`, `zdb_ts_event_chan`, `zdb_doc_event_chan`
  (payloads `zdb_kv_event_t`/`zdb_ts_event_t`/`zdb_doc_event_t`)
- `zdb_eventing_zbus_publish(event)` / `_publish_ts(event)` / `_publish_doc(event)`

Publication is best-effort and never changes operation return values.

## Time-Series

- `zdb_ts_open(db, stream_name, ts)` / `zdb_ts_close(ts)`
- `zdb_ts_append_i64(ts, sample)` / `zdb_ts_append_batch_i64(ts, samples, sample_count)`
- `zdb_ts_flush_async(ts)` / `zdb_ts_flush_sync(ts, timeout)`
- `zdb_ts_query_aggregate(ts, window, agg, out_result)` — MIN/MAX/AVG/SUM/COUNT
- `zdb_ts_recover_stream(ts, out_truncated_bytes)`
- `zdb_ts_watermark_set(ts, consumed_ts_ms)` / `zdb_ts_watermark_get(ts, out_consumed_ts_ms)` /
  `zdb_ts_watermark_clear(ts)`
- `zdb_ts_cursor_open(ts, window, predicate, predicate_ctx, out_cursor)`
- `zdb_ts_cursor_open_desc(ts, window, predicate, predicate_ctx, out_cursor)` — newest-first

Helpers: `ZDB_TS_WINDOW_ALL` — a `zdb_ts_window_t` covering all timestamps.

Notes:

- An instance holds up to `CONFIG_ZDB_TS_MAX_STREAMS` streams open at once
  (default 1). Opening one more than that returns `ZDB_ERR_BUSY`; closing a
  stream frees its slot. Re-opening a stream that is already open shares the
  same slot, and the slot is released once every handle is closed.
- Each open stream has its own ingest buffer, so appends to different streams
  stay independent. `zdb_ts_flush_async`/`zdb_ts_flush_sync` persist every
  stream's buffered samples, and `zdb_ts_close` flushes the stream it closes.
- Cursors iterate flushed records from storage plus samples still in the
  RAM ingest buffer. `zdb_cursor_reset` rewinds a cursor so it can be walked
  again.
- `zdb_ts_cursor_open_desc` walks the same records newest-first: unflushed
  samples first, then stored records from the end backwards. Order follows
  storage, which matches timestamp order when samples are appended as they are
  produced; a stream appended out of order comes back in reverse append order,
  not sorted. Unsupported on FCB, which can only walk forward.
- `zdb_ts_query_aggregate` scans at most `CONFIG_ZDB_TS_MAX_AGG_POINTS`
  samples for MIN/MAX/AVG/SUM; when more matched, the result covers only that
  prefix and `result.truncated` is set. Those aggregates return
  `ZDB_ERR_NOT_FOUND` for an empty window.
- COUNT is exempt: it is never capped and never truncated, and an empty window
  is `ZDB_OK` with `points == 0`. Counting the whole stream
  (`ZDB_TS_WINDOW_ALL`) derives the total from the stored size instead of
  reading payloads, so it stays cheap as the stream grows. It counts stored
  records, so a record that would fail its CRC is still counted; use a cursor
  if you need decode-verified totals.
- A **consumed watermark** lets a forwarder or uploader remember how far it
  processed a stream, so a restart resumes rather than replays. Acknowledge a
  batch with `zdb_ts_watermark_set`, then open the next cursor at
  `watermark + 1`. The value is stored rather than interpreted: it may move
  backwards to force re-processing, and it is independent of which samples the
  stream still holds. A watermark that fails its integrity check reads as
  unset, so a consumer replays instead of skipping.
- With `CONFIG_ZDB_TS_ROLLOVER` a stream is **bounded**: once it reaches
  capacity the oldest samples are discarded so new ones keep being accepted,
  which suits a long-running logger that wants a recent window rather than a
  full partition. Without it a full stream preserves what it holds and reports
  an error instead. On LittleFS the stream is stored as up to
  `CONFIG_ZDB_TS_ROLLOVER_MAX_SEGMENTS` segment files of about
  `CONFIG_ZDB_TS_ROLLOVER_SEGMENT_BYTES` each, and the oldest whole segment is
  what gets discarded; cursors, aggregates, and recovery span them. A stream
  previously stored as a single file is adopted as the first segment.
- `CONFIG_ZDB_TS_DELTA_ENCODING` stores each sample's timestamp as an offset
  from a base recorded once per segment and drops the per-record magic and
  version the segment header already carries, shrinking records from 28 bytes
  to 16 — about 1.75x the samples for the same storage. Records stay fixed size
  and independently decodable, so reverse cursors and the payload-free COUNT
  are unaffected. Streams written without it keep their format when appended
  to, and are still read; only new streams use the compact one. A sample more
  than 2^32 ms (about 49 days) past its segment's base starts a new segment
  rather than failing.
- Backend differences (FCB): appends are written through synchronously, so
  `flush_*` are no-ops returning `ZDB_OK`; `zdb_ts_query_aggregate` returns
  `ZDB_ERR_UNSUPPORTED`; `zdb_ts_recover_stream` is a no-op reporting zero
  truncated bytes; `zdb_ts_cursor_open_desc` and the watermark calls return
  `ZDB_ERR_UNSUPPORTED`.

## FlatBuffers Export

Serialize records for transport off-device. Stored data stays in ZephyrDB's own
format; these produce a copy. Both require `CONFIG_ZDB_FLATBUFFERS=y` and
`CONFIG_FLATCC=y`, which bring in the flatcc runtime.

- `zdb_ts_sample_i64_export_flatbuffer(sample, out_buf, out_capacity, out_len)`
- `zdb_doc_export_flatbuffer(doc, out_buf, out_capacity, out_len)` — carries the
  collection name, document ID, timestamps, and every field with its name,
  type, and value. The wire layout is documented as a schema in
  `zephyrdb_doc.c`, so a host can generate a matching reader.

Passing `out_buf = NULL` reports the required size in `*out_len` without
serializing into a buffer.

## Document Model

- `zdb_doc_create(db, collection_name, document_id, out_doc)`
- `zdb_doc_open(db, collection_name, document_id, out_doc)`
- `zdb_doc_save(doc)` / `zdb_doc_delete(db, collection_name, document_id)` / `zdb_doc_close(doc)`
- Field setters/getters for `i64`, `f64`, `string`, `bool`, `bytes`
  (`zdb_doc_field_set_*` / `zdb_doc_field_get_*`)
- `zdb_doc_query(db, query, out_metadata, out_count)`
- `zdb_doc_metadata_free(metadata, count)`
- `zdb_doc_export_flatbuffer(doc, out_buf, out_capacity, out_len)` — produces a
  FlatBuffer copy of a document for sending off-device; pass `out_buf = NULL`
  to ask how large a buffer it needs. Requires `CONFIG_ZDB_FLATBUFFERS`.

Notes:

- Fields hold `INT64`, `DOUBLE`, `STRING`, `BOOL`, or `BYTES` values. The
  `NULL`, `OBJECT`, and `ARRAY` enum values are reserved for future use and
  report `ZDB_ERR_UNSUPPORTED`; to store structured data today, serialize it
  into a `BYTES` field.
- `zdb_doc_query`: filters are AND-combined equality matches; `*out_count`
  is the output array capacity on input and the result count on output.
  Passing `out_metadata = NULL` returns the total match count. The query
  scans all collections; there is no collection filter field.
- Query time windows (`from_ms`/`to_ms`) filter on `updated_ms`, which is
  milliseconds since boot (`k_uptime_get()`), not wall-clock time.
- `zdb_doc_save` is atomic: it stages the document into `<id>.zdoc.tmp` and
  renames it over the live file, so an interrupted save leaves the previously
  stored document intact. If the live file is missing but a complete staging
  file is present, `zdb_doc_open` promotes it; an unparsable staging file is
  discarded by the next save or delete.
- The stored CRC covers the header **and** every field payload, so payload
  corruption or a truncated file is reported as `ZDB_ERR_CORRUPT` on open.
  Both the current format and the earlier header-only-CRC format are readable.

## Shell

- `zdb_shell_register(db)` — registers the `zdb` command tree
  (`zdb health`, `zdb stats`, `zdb kv set|get|delete|list`,
  `zdb ts append|query|flush`, `zdb doc open`). One instance at a time.
