# ZephyrDB DOC Basic Sample

This sample demonstrates document model APIs:
- `zdb_doc_create`
- `zdb_doc_field_set_*`
- `zdb_doc_save`
- `zdb_doc_export_flatbuffer`

Build example:

```bash
west build -p always -s samples/doc_basic -b <board_with_filesystem>
west build -t run
```

Notes:
- Requires filesystem setup at `CONFIG_ZDB_LFS_MOUNT_POINT`.
- Requires `flatcc-zephyr` module as a sibling workspace repo.
