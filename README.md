# ZephyrDB

Embedded multi-model database for Zephyr RTOS, designed for memory-constrained systems.

## What Is Implemented

- Zero-malloc static-slab memory model
- Key-value model with NVS or ZMS backends
- Time-series model with LittleFS or FCB backends
- Document model with typed fields and query APIs
- Durability helpers: integrity checks, recovery, stats export
- Optional FlatBuffers export helper for TS samples
- Optional KV event emitter/listener hooks
- Optional zbus adapter for KV event publication

## Quick Start

### 1. Add module to west manifest

```yaml
manifest:
  projects:
    - name: zephyrdb
      url: https://github.com/beriberikix/zephyrdb
      path: modules/lib/zephyrdb
      revision: main
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

## Documentation

- [Documentation Index](docs/README.md)
- [API Reference](docs/api.md)
- [Architecture](docs/architecture.md)
- [Configuration](docs/configuration.md)
- [Testing Guide](docs/testing.md)
- [Samples Guide](docs/samples.md)
- [Roadmap](docs/roadmap.md)

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
