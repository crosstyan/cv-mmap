Deterministic protocol fixtures generated from `cvmmap-core` C++ wire structs.

Files:
- `sync_valid.bin`: canonical `sync_message_t`
- `body_tracking_valid.bin`: canonical body-tracking packet
- `manifest.json`: expected decoded fields for downstream parser tests

Regeneration:

```bash
cmake -B build -S .
cmake --build build --target cvmmap_protocol_fixture_writer
./build/core/tests/cvmmap_protocol_fixture_writer --write core/fixtures/protocol
```

Verification:

```bash
ctest --test-dir build --output-on-failure -R cvmmap_protocol_fixture_verify
```
