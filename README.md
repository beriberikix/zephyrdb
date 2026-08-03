# ZephyrDB

[![docs](https://img.shields.io/github/actions/workflow/status/beriberikix/zephyrdb/docs.yml?branch=main&label=docs)](https://beriberikix.github.io/zephyrdb/)
[![tests](https://img.shields.io/github/actions/workflow/status/beriberikix/zephyrdb/test.yml?branch=main&label=tests)](https://github.com/beriberikix/zephyrdb/actions/workflows/test.yml)

Embedded multi-model database for Zephyr RTOS, designed for memory-constrained systems.

## Features

**Key-value** — namespaced records on NVS or ZMS, with byte and string APIs.
A defaults table seeds product parameters at init and merges in newly added
keys after a firmware update without disturbing values the device changed.
Iteration covers every stored key, including those written before the current
boot, and a namespace can be reset to its defaults for factory reset.

**Time-series** — append-only streams on LittleFS or FCB, buffered in RAM and
flushed in the background. Cursors walk a time window in either direction,
aggregates compute MIN/MAX/AVG/SUM/COUNT, and a consumed watermark records how
far a forwarder has processed so a restart resumes rather than replays.
Streams can be bounded, discarding the oldest samples to keep a recent window,
and can store compact records that hold about 1.75x the samples per byte.
Several streams can be open at once.

**Documents** — typed fields (integer, double, string, bool, bytes) with
equality-filter queries, stored on a filesystem. Saves are atomic and
integrity-checked end to end, so an interrupted write leaves the previous
document intact rather than a truncated one.

**Across all models** — bounded, statically sized allocation on the core and
time-series paths; corruption detection with recovery; optional mutation
events, with a zbus adapter; optional FlatBuffers export for transport; and a
[`zdb` shell command tree](docs/shell.md).

## Quick Start

### 1. Add module to west manifest

```yaml
manifest:
  projects:
    - name: zephyrdb
      url: https://github.com/beriberikix/zephyrdb
      path: modules/lib/zephyrdb
      revision: v0.7.0
```

### 2. Enable in prj.conf

```conf
CONFIG_ZEPHYRDB=y

# Optional models
CONFIG_ZDB_KV=y
CONFIG_ZDB_TS=y
CONFIG_ZDB_DOC=y

# Example backends
CONFIG_ZDB_KV_BACKEND_NVS=y
CONFIG_ZDB_TS_BACKEND_LITTLEFS=y

# Optional stats and export helpers
CONFIG_ZDB_STATS=y
CONFIG_FLATCC=y
CONFIG_ZDB_FLATBUFFERS=y
```

### 3. Initialize

```c
#include <zephyrdb.h>

static const zdb_cfg_t cfg = {
    .kv_backend_fs = NULL,
    .lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
    .work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(db, cfg);

int rc = zdb_init(&db, &cfg);
if (rc != ZDB_OK) {
    /* handle init error */
}
```

For a complete standalone application example, see [zephyrdb-example](https://github.com/beriberikix/zephyrdb-example).

## Documentation

- [Documentation Index](docs/README.md)
- [API Reference](docs/api.md)
- [Architecture](docs/architecture.md)
- [Configuration](docs/configuration.md)
- [Testing Guide](docs/testing.md)
- [Samples Guide](docs/samples.md)
- [Shell Guide](docs/shell.md)

Planned and declined enhancements are tracked as
[GitHub issues](https://github.com/beriberikix/zephyrdb/issues).

## Eventing and zbus Adapter

ZephyrDB can report what it is doing, in four categories:

| Category | Payload | Events |
|---|---|---|
| Key-value | `zdb_kv_event_t` | set, delete, index-full |
| Time-series | `zdb_ts_event_t` | append, flush, recover, rollover, watermark |
| Document | `zdb_doc_event_t` | create, save, delete |
| Instance | `zdb_core_event_t` | init, deinit, health change |

- `CONFIG_ZDB_EVENTING=y` delivers them to listener callbacks registered
  through `zdb_cfg_t` before `zdb_init()`. Callbacks run in the calling
  thread, never with an internal lock held, and must not block.
- `CONFIG_ZDB_EVENTING_ZBUS=y` additionally publishes each event on its own
  zbus channel — `zdb_kv_event_chan`, `zdb_ts_event_chan`,
  `zdb_doc_event_chan`, `zdb_core_event_chan` — for subscribers elsewhere in
  the application. Publication is best-effort and never changes an
  operation's return value.

Events carry the operation's real status, so failures are reported as well as
successes. They are worth wiring up for the things that are otherwise
invisible: a bounded stream discarding its oldest samples, an instance
degrading after corruption, or a key that stored fine but no longer fits the
iteration index.

See [docs/api.md](docs/api.md#eventing) and the sample:

- [samples/eventing_zbus](samples/eventing_zbus)

## Storage Layout

- KV backend storage is provided through `cfg.kv_backend_fs`
- TS files live under `<mount>/<CONFIG_ZDB_TS_DIRNAME>/`
- DOC files live under `<mount>/zdb_docs/`

Use dedicated flash partitions when isolating ZephyrDB from app settings storage.

## Build Integration

ZephyrDB integrates through:

- `CMakeLists.txt`
- `Kconfig` and `Kconfig.zephyrdb`
- `module.yml`

## License

See [LICENSE](LICENSE).
