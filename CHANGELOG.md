# Changelog

Notable changes per release. ZephyrDB is 0.x: the API may change between minor
releases. On-disk formats are versioned and readers accept older ones.

## v0.7.0 — events-focused refresh

Eventing grew from key-value only to cover every model, but the documentation
and the tests did not follow. This release makes what ZephyrDB reports match
what it does, puts the whole surface under CI, and reports the state changes
recent features had been making silently.

### Added

- **Instance events.** A fourth listener category, `zdb_core_event_listener_t`
  / `zdb_core_event_t`, registered through `zdb_cfg_t.core_event_listeners`.
  Reports `ZDB_CORE_EVENT_INIT`, `ZDB_CORE_EVENT_DEINIT` and
  `ZDB_CORE_EVENT_HEALTH`, the last carrying `prev_health` and `health`.
  Health was previously observable only by polling `zdb_health()`.
- `ZDB_TS_EVENT_ROLLOVER` — a bounded stream discarding its oldest segments,
  with the bytes dropped in `truncated_bytes`. This is the only place ZephyrDB
  destroys data on purpose, and it used to do so silently.
- `ZDB_TS_EVENT_WATERMARK` — a consumed watermark set or cleared, reporting the
  position in `sample_ts_ms`.
- `ZDB_EVENT_KV_INDEX_FULL` — a key that stored successfully but did not fit
  `CONFIG_ZDB_KV_INDEX_MAX_ENTRIES`. The value stays readable by name; only
  enumeration is lost. Previously silent.
- `zdb_core_event_chan` and `zdb_eventing_zbus_publish_core()`.
- Test suites `unit/core_events`, `unit/doc_events` and `unit/eventing_zbus` —
  the zbus adapter had no automated coverage of any kind before this.

### Fixed

- **Health could never degrade with `CONFIG_ZDB_STATS=n`.** `zdb_health_check()`
  decided from the durability counters, which compile away when statistics are
  off, so such builds reported `ZDB_HEALTH_OK` no matter how much corruption
  they met. Degradation is now tracked independently of the counters.
- **Document events never reported failures.** `zdb_doc_save()`,
  `zdb_doc_delete()` and `zdb_doc_create()` passed a hardcoded `ZDB_OK` and
  returned before the emit on every error path, so a listener could not tell a
  failed save from one that never happened. They now carry the real status.
- **Time-series recovery never reported failures.** Ten early returns in
  `zdb_ts_recover_stream()` emitted nothing; all now report through one exit.
- **Flush events undercounted.** Only the periodic work-queue drain emitted, so
  flushes caused by `zdb_ts_close()` or by a full ingest buffer during append
  were invisible and `flushed_bytes` did not reflect what reached storage.
  Every flush path now reports.
- **`zdb_deinit()` could crash after an append.** The time-series context was
  freed without settling a queued flush, leaving the work queue to run a
  handler in memory that had since been reused. It is now cancelled first.
- `samples/eventing_zbus` had no `sample.yaml`, so Twister never built it —
  the reason this drift went unnoticed. It now builds in CI, mounts LittleFS so
  its time-series and document paths actually run, and fails rather than
  silently skipping them.

### Changed

- `CONFIG_ZDB_EVENTING` now depends on `ZDB_CORE` rather than requiring a data
  model, since instance events are model-independent.
- Event callbacks are documented as never running while an internal lock is
  held, so a listener may call back into the same instance. Flush, rollover and
  health events detected inside locked sections are published on release.
- Health is documented as one-way and derived from time-series integrity
  checks; nothing restores an instance to `ZDB_HEALTH_OK`.
- README, `docs/api.md` and `docs/configuration.md` rewritten around the four
  categories. The README described eventing as key-value only, and is also the
  Doxygen mainpage, so that text was the published API site's definition.

## v0.6.0

Operator-complete `zdb` shell command tree. See the GitHub release notes.

## v0.5.0

First tagged release. Requires Zephyr v4.4.1.
