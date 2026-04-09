# eventing_zbus

This sample demonstrates ZephyrDB KV event emission bridged to zbus.

What it does:
- Initializes a ZMS-backed KV database on native_sim storage partition
- Performs one KV set and one KV delete
- Reads and prints the latest event payload from the zbus channel

Build and run:

```bash
west build -p always -s samples/eventing_zbus -b native_sim
west build -t run
```

Expected output includes lines similar to:
- `event after set: type=SET ...`
- `event after delete: type=DELETE ...`

Full sample catalog: [../../docs/samples.md](../../docs/samples.md)
