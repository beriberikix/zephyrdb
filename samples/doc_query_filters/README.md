# ZephyrDB DOC Query Filters Helper Sample

This helper sample demonstrates practical filter construction for `zdb_doc_query`.

It shows:
- seeding a document
- building multiple query filters (string + bool)
- using result limits and time windows
- collecting metadata matches

Build example:

```bash
west build -p always -s samples/doc_query_filters -b <board_with_filesystem>
west build -t run
```
