# Roadmap

Unimplemented enhancements under consideration. Items are removed as they ship.

Tiers reflect a value-vs-effort assessment (2026-08). Dependencies are noted
inline. Shipped since the last revision and therefore removed: cursor-based TS
time-range iteration (`zdb_ts_cursor_open()` + `zdb_ts_window_t`), except
reverse traversal, which remains below.

## Now — correctness fixes and low-effort, high-value

- **Atomic document saves** — `zdb_doc_save()` currently truncates the live
  file in place (`FS_O_TRUNC`), so a power loss mid-save corrupts the existing
  document; write-to-temp-then-rename (atomic on LittleFS) fixes this. Extend
  the header CRC to cover field payloads. This is a data-integrity fix and
  should land before new features.
- **KV default values with auto-init and incremental upgrade** — Accept a
  default KV table in `zdb_cfg_t`; on every init, write any default whose key
  is absent. Because the pass is write-if-missing, it doubles as the
  post-firmware-update merge: new defaults appear, user-modified values
  survive. (Handling removed/renamed/retyped keys would additionally need a
  defaults-schema version; defer until needed.)
- **TS COUNT correctness and cost** — `ZDB_TS_AGG_COUNT` exists but decodes
  every payload and silently truncates at `CONFIG_ZDB_TS_MAX_AGG_POINTS`,
  returning a low count as if complete. Report or remove the truncation for
  COUNT, and count fixed-size records without payload decode (full-window
  count on the file backend is `size / record_size`).
- **KV string type convenience API** — Dedicated string get/set wrappers over
  the raw blob API (store NUL, guarantee termination on read). Trivial;
  filler work.

## Next — structural features

- **Persistent KV iteration** — The v2 record format already stores the
  namespace on disk; a one-time backend ID scan at open can rebuild the key
  index across reboots (today iteration covers only keys touched this
  session). Prerequisite for reset-to-defaults.
- **KV reset to defaults** — Factory-reset API: enumerate and delete all keys
  in a namespace, then re-run the defaults pass. Depends on persistent KV
  iteration for enumeration.
- **TS rollover / circular buffer mode** — Explicit, backend-agnostic
  overwrite-oldest-when-full option. FCB nearly provides it via sector
  rotation; the LittleFS backend needs segmented files with oldest-segment
  deletion. Highest-value TS feature; medium effort.
- **TS consumed watermark** — Persisted "processed up to timestamp T" ack
  state per stream, serving mark-as-processed upload pipelines. Replaces the
  earlier per-record mutable status idea, which fights both backends (FCB is
  append-only; LittleFS copy-on-write makes in-place mutation illusory).
- **TS multiple concurrent streams** — Lift the one-stream-per-instance limit
  (second `zdb_ts_open()` returns `ZDB_ERR_BUSY` today); N streams, each with
  its own file/FCB region, slab-sized via Kconfig. Subsumes the earlier
  "multi-stream coordination APIs" item — coordination can follow if a
  concrete need appears.
- **TS reverse cursor traversal** — Descending iteration; easy on the file
  backend (fixed-size records, seek backward), initially unsupported on FCB
  (forward-walk API).
- **FlatBuffers document export** — `zdb_doc_export_flatbuffer()` is a stub
  returning `ZDB_ERR_UNSUPPORTED`; the flatcc dependency is already available
  as a sibling Zephyr module (`flatcc-zephyr`).

## Later — larger or niche

- **Nested object/array document fields** — `ZDB_DOC_FIELD_OBJECT`/`ARRAY`
  exist in the type enum but serialization is unimplemented. Large API and
  serialization surface that pressures the no-heap design goal. Interim:
  document a CBOR-in-`BYTES` pattern in a sample.
- **NVS/ZMS document backends** — For boards without a filesystem, with a RAM
  manifest for query enumeration. Interim: a sample showing serialized
  documents stored as KV blobs covers most of the need at a fraction of the
  cost.
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
