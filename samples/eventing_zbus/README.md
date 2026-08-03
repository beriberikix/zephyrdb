# eventing_zbus

Bridges ZephyrDB events onto zbus channels and reads each one back, across all
four event categories.

What it does:
- Initializes a ZMS-backed KV database and a LittleFS mount on native_sim, then
  reads the instance event that `zdb_init()` published
- Performs KV set/delete and reads `zdb_kv_event_chan`
- Performs TS append/flush and sets a consumed watermark, reading
  `zdb_ts_event_chan` after each
- Performs DOC create/save/delete and reads `zdb_doc_event_chan`
- Tears the instance down and reads the closing instance event from
  `zdb_core_event_chan`

Every step is a hard failure: the board overlay supplies the `/lfs` partition
the time-series and document models need, so a skipped step would mean the
sample was demonstrating nothing.

Build and run:

```bash
west build -p always -s samples/eventing_zbus -b native_sim
west build -t run
```

Expected output includes lines similar to:
- `eventing_zbus: event after init type=INIT health=OK ...`
- `eventing_zbus: event after kv set type=SET ...`
- `eventing_zbus: event after kv delete type=DELETE ...`
- `eventing_zbus: event after ts append type=APPEND ...`
- `eventing_zbus: event after ts flush type=FLUSH ...`
- `eventing_zbus: event after ts watermark type=WATERMARK ...`
- `eventing_zbus: event after doc save type=SAVE ...`
- `eventing_zbus: event after doc delete type=DELETE ...`
- `eventing_zbus: event after deinit type=DEINIT ...`
- `eventing_zbus: PASS`

The sample reads channels directly with `zbus_chan_read()`, which returns the
message a channel is holding. An application that wants to be woken instead
should attach a zbus observer to the channel.

Full sample catalog: [../../docs/samples.md](../../docs/samples.md)
