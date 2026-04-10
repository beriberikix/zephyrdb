# eventing_zbus

# eventing_zbus

This sample demonstrates ZephyrDB KV, TS, and DOC event emission bridged to zbus.

What it does:
- Initializes a ZMS-backed KV database on native_sim storage partition
- Performs KV set/delete and reads from `zdb_kv_event_chan`
- Performs TS append/flush (when filesystem is available) and reads from `zdb_ts_event_chan`
- Performs DOC create/save/delete and reads from `zdb_doc_event_chan`

Build and run:

```bash
west build -p always -s samples/eventing_zbus -b native_sim
west build -t run
```

Expected output includes lines similar to:
- `event after kv set: type=SET ...`
- `event after kv delete: type=DELETE ...`
- `event after ts append: type=APPEND ...` (if filesystem available)
- `event after ts flush: type=FLUSH ...` (if filesystem available)
- `event after doc create: type=CREATE ...`
- `event after doc save: type=SAVE ...` (if filesystem available)
- `event after doc delete: type=DELETE ...` (if filesystem available)

Full sample catalog: [../../docs/samples.md](../../docs/samples.md)

Build and run:

```bash
west build -p always -s samples/eventing_zbus -b native_sim
west build -t run
```

Expected output includes lines similar to:
- `event after set: type=SET ...`
- `event after delete: type=DELETE ...`

Full sample catalog: [../../docs/samples.md](../../docs/samples.md)
