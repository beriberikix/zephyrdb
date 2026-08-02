# Roadmap

Unimplemented enhancements under consideration. Items are removed as they ship.

Tiers reflect a value-vs-effort assessment (2026-08). Dependencies are noted
inline. Shipped since the last revision and therefore removed: cursor-based TS
time-range iteration (`zdb_ts_cursor_open()` + `zdb_ts_window_t`), except
reverse traversal, which remains below.

## Next — structural features

- **TS rollover / circular buffer mode** — Explicit, backend-agnostic
  overwrite-oldest-when-full option. FCB nearly provides it via sector
  rotation; the LittleFS backend needs segmented files with oldest-segment
  deletion. Highest-value TS feature; medium effort.
- **TS multiple concurrent streams** — Lift the one-stream-per-instance limit
  (second `zdb_ts_open()` returns `ZDB_ERR_BUSY` today); N streams, each with
  its own file/FCB region, slab-sized via Kconfig. Subsumes the earlier
  "multi-stream coordination APIs" item — coordination can follow if a
  concrete need appears.
- **FlatBuffers document export** — `zdb_doc_export_flatbuffer()` is a stub
  returning `ZDB_ERR_UNSUPPORTED`; the flatcc dependency is already available
  as a sibling Zephyr module (`flatcc-zephyr`).

## Later — larger or niche

- **Nested object/array document fields** — `ZDB_DOC_FIELD_OBJECT`/`ARRAY`
  exist in the type enum but serialization is unimplemented. Large API and
  serialization surface that pressures the no-heap design goal. Interim:
  document a CBOR-in-`BYTES` pattern in a sample.
- **NVS/ZMS document backends** — For boards without a filesystem, with a RAM
  manifest for query enumeration. The `doc_kv_blob` sample now demonstrates the
  interim pattern (versioned struct stored as one KV value, seeded by the
  defaults table, cleared by namespace reset), which covers most of the need at
  a fraction of the cost.
- **TS timestamp delta encoding** — Bounded compression (v2 record format
  with delta-encoded timestamps) for 2–3× capacity. Full compression or
  long-window compaction is not worth the CPU/RAM on-device.
- **Encryption at rest** — Real product need but pulls in key management
  (PSA/TF-M). Until a concrete requirement lands, document Zephyr's
  flash-encryption options instead.

## Dropped / out of scope

- **Runtime sector/partition size control** — KV geometry is already
  app-owned at runtime via `cfg.kv_backend_fs`; only the FCB sector count is
  Kconfig-bound, and moving it to `zdb_cfg_t` is not worth the churn until a
  use case appears.
- **Extended document schema evolution** — The document header already
  carries a format version; this is a compatibility policy to document, not a
  feature to build.
- **Distributed edge synchronization** — Out of scope for a local embedded
  database core; if pursued, it belongs in an application-layer sample or a
  separate module.
