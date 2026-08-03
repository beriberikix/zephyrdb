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
      revision: v0.6.0
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

ZephyrDB can emit lightweight KV mutation events when enabled:

- `CONFIG_ZDB_EVENTING=y` enables local KV event emission
- `CONFIG_ZDB_EVENTING_ZBUS=y` bridges those events to a zbus channel

The zbus channel carries `zdb_kv_event_t` messages and is intended as an
optional adapter layer for local subscribers.

See sample:

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
