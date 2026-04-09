# Configuration

ZephyrDB is configured through [../Kconfig.zephyrdb](../Kconfig.zephyrdb).

## Core Options

- `CONFIG_ZEPHYRDB`: enable ZephyrDB core
- `CONFIG_ZDB_STATS`: enable runtime statistics
- `CONFIG_ZDB_SCAN_YIELD_EVERY_N`: cooperative scan yield frequency

## Eventing Options

- `CONFIG_ZDB_EVENTING`: enable local KV mutation events
- `CONFIG_ZDB_EVENTING_ZBUS`: publish KV mutation events to zbus

Notes:

- `CONFIG_ZDB_EVENTING_ZBUS` depends on `CONFIG_ZDB_EVENTING` and `CONFIG_ZBUS`.
- Event publication is best-effort and does not alter KV write/delete return values.

## Key-Value Options

- `CONFIG_ZDB_KV`: enable KV module
- `CONFIG_ZDB_KV_BACKEND_NVS`: use NVS backend
- `CONFIG_ZDB_KV_BACKEND_ZMS`: use ZMS backend

## Time-Series Options

- `CONFIG_ZDB_TS`: enable TS module
- `CONFIG_ZDB_TS_BACKEND_LITTLEFS`: use LittleFS backend
- `CONFIG_ZDB_TS_BACKEND_FCB`: use FCB backend
- `CONFIG_ZDB_TS_INGEST_BUFFER_BYTES`: TS ingest buffer size
- `CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN`: recovery on open
- `CONFIG_ZDB_LFS_MOUNT_POINT`: mount point for filesystem-backed storage
- `CONFIG_ZDB_TS_DIRNAME`: TS directory name under mount point

## FlatBuffers and Document Options

- `CONFIG_FLATCC`: enable FlatCC runtime support
- `CONFIG_ZDB_FLATBUFFERS`: enable FlatBuffers helper support
- `CONFIG_ZDB_DOC`: enable document model APIs
- `CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN`: max document field name length
- `CONFIG_ZDB_DOC_MAX_STRING_LEN`: max string field size
- `CONFIG_ZDB_DOC_MAX_FIELD_COUNT`: max field count per document
- `CONFIG_ZDB_DOC_MAX_NESTED_DEPTH`: max nested document depth

## Practical Notes

- Use board overlays to enable flash/filesystem features required by your backend.
- For backend validation, prefer boards with real flash support.
