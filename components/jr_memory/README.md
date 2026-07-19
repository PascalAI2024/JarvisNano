# jr_memory

Small NVS-backed fact memory for the v5 device.

- 12 keyed facts, with 32-byte keys and 192-byte values.
- Two CRC-checked snapshot slots. A mutation commits the inactive slot before
  replacing RAM state, so a torn write falls back to the previous generation.
- Fixed storage and output limits; no silent eviction or JSON truncation.
- Mutex-serialized access after idempotent initialization.
- Conservative credential classifier. Fact content is never logged.

`jr_memory_recall()` and `jr_memory_list()` produce complete JSON, ready for a
tool response or authenticated diagnostics endpoint. `jr_memory_store()`
supports the `remember` tool's keyed fact semantics. NVS must already be
initialized before `jr_memory_init()`.

The guard is defense in depth. Facts remain plaintext unless NVS encryption is
enabled, so callers must still refuse sensitive personal data and credentials.

Run the portable guard tests without ESP-IDF:

```sh
cmake -S components/jr_memory/tests -B /tmp/jr-memory-tests
cmake --build /tmp/jr-memory-tests
ctest --test-dir /tmp/jr-memory-tests --output-on-failure
```
