# Shell Guide

ZephyrDB registers a `zdb` command tree into the standard Zephyr shell, so
the commands appear over whichever backend the application already uses —
UART, RTT, or telnet — alongside `kernel`, `device`, and `flash`.

Enable it with `CONFIG_ZDB_SHELL=y` (requires `CONFIG_SHELL=y`), then bind an
instance from the application after `zdb_init()`:

```c
zdb_shell_register(&db);
```

Until that call, every command reports that no instance is registered and
returns `-ENODEV`. One instance is bound at a time; a later call replaces the
binding, and `NULL` unbinds. `samples/shell_basic` is a complete working
example.

## Command Reference

Which subtrees exist follows the enabled models: `zdb kv` needs
`CONFIG_ZDB_KV`, `zdb ts` needs `CONFIG_ZDB_TS`, and `zdb doc` needs
`CONFIG_ZDB_DOC`. `zdb health`, `zdb info`, and `zdb stats` are always
present.

Every token before the first free-form operand is a fixed keyword, so tab
completion reaches the end of each command's keywords, and `-h` on any node
prints its usage.

### Core

- `zdb health` — report instance health (`OK`/`DEGRADED`/`READONLY`/`FAULT`)
- `zdb info` — report the version, enabled models, active backends, and the
  compile-time bounds an operator will hit
- `zdb stats show` — print the time-series durability counters
- `zdb stats reset` — zero the counters
- `zdb stats export` — print the packed, CRC-protected counter export as hex
  and validate it, which is the byte string to paste into a bug report

### Key-value

- `zdb kv get <namespace> <key>` — read a value
- `zdb kv list <namespace> [limit]` — list keys with a value preview
- `zdb kv delete <namespace> <key>` — delete a key
- `zdb kv set str <namespace> <key> <text>` — store text with its terminator
- `zdb kv set raw <namespace> <key> <text>` — store text without a terminator
- `zdb kv set hex <namespace> <key> <hexdigits>` — store decoded bytes
- `zdb kv reset <namespace>` — delete every key, then re-apply the defaults
- `zdb kv defaults` — write any missing entries of the defaults table

### Time-series

- `zdb ts append <stream> <ts_ms> <value>` — append one sample
- `zdb ts read <stream> [limit] [from_ms to_ms]` — walk samples oldest-first
- `zdb ts tail <stream> [limit] [from_ms to_ms]` — walk samples newest-first
- `zdb ts agg <stream> <min|max|avg|sum|count> [from_ms to_ms]` — aggregate
- `zdb ts flush sync <stream> [timeout_ms]` — flush and wait
- `zdb ts flush async <stream>` — queue a flush on the work queue
- `zdb ts recover <stream>` — scan and truncate a corrupt tail
- `zdb ts watermark get|set|clear <stream> [ts_ms]` — consumer watermark

### Documents

- `zdb doc create <collection> <document_id>` — create and save an empty
  document; refuses to overwrite an existing one
- `zdb doc delete <collection> <document_id>` — delete a document
- `zdb doc get <collection> <document_id> [field]` — print every field, or one
- `zdb doc list [limit] [from_ms to_ms]` — list documents across collections
- `zdb doc set i64|f64|str|bool|bytes <collection> <document_id> <field> <value>`
  — set a typed field and save
- `zdb doc find i64|f64|str|bool <field> <value> [limit]` — find documents
  whose field equals a value

`set` names a type because a shell value is untyped text. `get` does not,
because the stored field already carries one — and the operator usually does
not know it.

## Value Encoding

A bare token is ambiguous: `0xff` is either a four-character string or a
single byte, and guessing silently corrupts data. So `zdb kv set` names the
encoding, and `zdb kv set <TAB>` lists the three choices:

| Subcommand | Stored bytes |
|---|---|
| `set str` | the text plus its NUL terminator |
| `set raw` | the text with no terminator |
| `set hex` | the decoded hex digits |

Hex input takes an even number of digits with an optional `0x` prefix. An odd
count is rejected rather than zero-padded, because padding would change the
value with no way to know which end was meant.

`zdb kv get` always prints a `hex:` line, and adds a `str:` line when the
bytes are printable ASCII. The `hex:` line is exactly what `zdb kv set hex`
accepts, so any value round-trips:

```
uart:~$ zdb kv set str cfg name kitchen
status: ok
namespace: cfg
key: name
bytes: 8

uart:~$ zdb kv get cfg name
namespace: cfg
key: name
len: 8
truncated: no
hex: 6b69746368656e00
str: kitchen
```

`zdb doc set bytes` uses the same hex convention.

## Output Format

Four line shapes, one scheme:

- **Field line** — `name: value`, for scalar results and for the operands a
  mutation echoes back.
- **Record line** — `[<index>] <key>=<value> …`, one per row of a listing.
  Space-separated rather than column-aligned, so a long key cannot break the
  layout and every row stays greppable.
- **Summary line** — closes a listing with `shown:`, plus `truncated:` when a
  limit could have been hit and `matched:` when the true total is known.
- **Error line** — `error: <operation> failed: <STATUS>`, sometimes followed
  by a `hint:` line.

Mutations print `status: ok` and the operands they acted on; silence on
success reads as a hang on a serial console.

```
uart:~$ zdb ts read temp 2
stream: temp
order: asc
[0] ts_ms=1000 value=20
[1] ts_ms=1001 value=21
shown: 2
truncated: yes
```

### Exit Status

Commands return a negative errno that matches the status they printed:

| Status | Return |
|---|---|
| `ZDB_OK` | `0` |
| `ZDB_ERR_INVAL` | `-EINVAL` |
| `ZDB_ERR_NOMEM` | `-ENOMEM` |
| `ZDB_ERR_NOT_FOUND` | `-ENOENT` |
| `ZDB_ERR_BUSY` | `-EBUSY` |
| `ZDB_ERR_TIMEOUT` | `-ETIMEDOUT` |
| `ZDB_ERR_UNSUPPORTED` | `-ENOTSUP` |
| `ZDB_ERR_CORRUPT` | `-EBADMSG` |
| `ZDB_ERR_COLLISION` | `-EEXIST` |
| `ZDB_ERR_IO`, `ZDB_ERR_INTERNAL` | `-EIO` |

Two conventions worth knowing:

- **A missing item is an error for a lookup, not for a query.** `zdb kv get`,
  `zdb doc get`, and `zdb ts watermark get` return `-ENOENT` when the item is
  absent. `zdb ts agg` over an empty window prints `points: 0` and returns
  `0`, because "no samples in that window" is an answer.
- **No instance bound** returns `-ENODEV` from every command.

## Backend Differences

The FCB time-series backend writes through and keeps no sidecar state, so
part of the `zdb ts` surface reports `UNSUPPORTED` on it. `zdb info` names
the active backend.

| Command | LittleFS | FCB |
|---|---|---|
| `ts append`, `ts read` | yes | yes |
| `ts flush sync`, `ts flush async` | yes | accepted, but a no-op |
| `ts agg` | yes | `UNSUPPORTED` |
| `ts tail` | yes | `UNSUPPORTED` (forward-only) |
| `ts watermark get\|set\|clear` | yes | `UNSUPPORTED` |

`zdb ts flush sync <stream> 0` does not wait at all, so it reports `BUSY`
unless the flush finishes immediately. Pass `forever` to wait indefinitely.

## Configuration Notes

- **`CONFIG_CBPRINTF_FP_SUPPORT=y` is required** by `zdb ts agg`, `zdb doc
  get` on a `f64` field, and `zdb doc find f64`, all of which print doubles.
  Its `default y if FPU` does not fire on every target.
- `zdb doc set f64` and `zdb doc find f64` parse with `strtod`, which the
  minimal libc does not provide; picolibc and newlib both do.
- **`CONFIG_HEAP_MEM_POOL_SIZE` must fit the KV key index**, which is sized by
  `CONFIG_ZDB_KV_INDEX_MAX_ENTRIES` and `CONFIG_ZDB_MAX_KEY_LEN`. When it does
  not fit, `zdb kv list` reports `NOMEM` rather than an empty namespace.
- `zdb kv get` reads into a buffer sized to `CONFIG_ZDB_KV_IO_SLAB_BLOCK_SIZE`,
  which bounds any storable record, so it never truncates a stored value.
  `zdb kv list` previews the first 16 bytes of each value and marks a clipped
  preview with `..`; `zdb kv get` is the detail view.
- Hex input is bounded by half of `CONFIG_SHELL_CMD_BUFF_SIZE`, since N bytes
  take 2N typed characters. `zdb info` reports the limit as `hex_input_max`.
- Every command opens its stream or document, acts, and closes again before
  returning, so a single-stream build (`CONFIG_ZDB_TS_MAX_STREAMS=1`, the
  default) can still run every `zdb ts` command in any order.

## Limits

- **No tab completion for namespaces, streams, or collections.** There is no
  API to enumerate KV namespaces or time-series streams — a namespace exists
  only as a hashed component of a record ID, and streams are files whose path
  construction is internal. Documents are enumerable, but completion runs
  synchronously on every keystroke and a filesystem scan there would stall
  line editing. Use `zdb kv list`, `zdb doc list`, and `zdb info` instead. If
  a collection-listing API is added, document completion becomes cheap.
- **No `zdb doc unset`** — the document API has no field-delete call. To drop
  a field, read the document, delete it, create it again, and re-set the
  fields to keep.
- **No `zdb doc find bytes`** — document query filters carry no byte span.
- `zdb doc find i64` matches through a `double`, so integers above 2^53 lose
  precision.
- `zdb doc list` and `zdb doc find` report at most 16 rows per invocation and
  say `truncated: yes` when they hit that bound.
- FlatBuffers export and batch append have no command; neither has a
  reasonable console form. Use the C API.

## Related

- [Documentation Index](README.md)
- [API Reference](api.md)
- [Configuration](configuration.md)
- [Samples Guide](samples.md)
- [Project README](../README.md)
