Deterministic protocol fixtures generated from `cvmmap-core` C++ wire structs.

Files:
- `sync_valid.bin`: canonical `sync_message_t`
- `control_request_get_source_info.bin`: canonical `GET_SOURCE_INFO` control request
- `control_response_get_source_info.bin`: canonical `GET_SOURCE_INFO` success response
- `control_request_seek_timestamp_ns.bin`: canonical `SEEK_TIMESTAMP_NS` control request
- `control_response_seek_timestamp_ns.bin`: canonical `SEEK_TIMESTAMP_NS` success response
- `control_request_start_recording.bin`: canonical `START_RECORDING` control request
- `control_request_stop_recording.bin`: canonical `STOP_RECORDING` control request
- `control_request_get_recording_status.bin`: canonical `GET_RECORDING_STATUS` control request
- `control_response_recording_status.bin`: canonical recording-status success response
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
