
---

## 2026-03-04: ZED Config Validation in app_config.cpp (Task 3)

### Implementation Summary

Added strict validation for `zed.resolution` and `zed.depth_mode` in `app/config/app_config.cpp` during config parsing phase (not backend open phase).

### Functions Added

```cpp
std::string validate_and_canonicalize_zed_resolution(const std::string_view resolution);
std::string validate_and_canonicalize_zed_depth_mode(const std::string_view depth_mode);
```

### Resolution Validation

Valid canonical values: HD2K, HD1200, HD1080, HD720, SVGA, VGA, AUTO
Aliases accepted:
- "2k" → HD2K
- "1080p", "fhd" → HD1080
- "720p", "hd" → HD720

### Depth Mode Validation

Valid values: NONE, PERFORMANCE, QUALITY, ULTRA
No aliases (values are canonicalized to uppercase).

### Validation Behavior

- Invalid values throw `std::invalid_argument` with clear error messages listing allowed values
- Case-insensitive matching (normalized via `normalize_ascii_lower`)
- Values stored in canonical uppercase form
- Existing stream_mode/network constraints preserved

### Test Results

```bash
# Invalid resolution - exits with code 1
./cv-mmap -c invalid_resolution.toml
# Error: invalid zed.resolution: '4K'. Allowed values: HD2K, HD1200, HD1080, HD720, SVGA, VGA, AUTO (aliases: 2k, 1080p, fhd, 720p, hd)

# Invalid depth_mode - exits with code 1
./cv-mmap -c invalid_depth_mode.toml
# Error: invalid zed.depth_mode: 'neural'. Allowed values: NONE, PERFORMANCE, QUALITY, ULTRA

# Valid aliases - config parsed successfully
./cv-mmap -c valid_aliases.toml
# Proceeds to backend selection (ZED unavailable in non-ZED build is expected)
```

### Header Added

```cpp
#include <unordered_set>
```

---

## 2026-03-04: Task 4 Producer SHM ABI v2 Write Path (src/main.cpp)

### Implementation Summary

Updated producer shared-memory write path to emit `frame_metadata_v2_t` in metadata bytes `[0..255]` while preserving existing sync/control flow.

### Behavior Implemented

- SHM metadata region is treated as `frame_metadata_v2_t` and written with `memcpy`.
- Header fields now set per v2 contract on each frame:
  - magic via `header.ensure_magic()`
  - `versions_major = 2`, `versions_minor = VERSION_MINOR`
  - `frame_id = metadata.frame_count`
  - `capture_ts_ns = metadata.timestamp_ns`
  - `publish_seq = metadata.frame_count`
  - `plane_descriptors_offset = 64`
  - `plane_descriptor_size = 24`
  - `plane_descriptor_capacity = 4`
  - `payload_size_bytes = frame_buffer.size()`
- Current backend-compatible active plane policy implemented:
  - `plane_count = 1`
  - `plane_presence_mask = 0x01`
  - slot0 LEFT descriptor filled from `metadata.info`
  - slot1..slot3 remain zero (metadata struct zero-initialized first)
- Payload copy writes frame bytes to payload offset 0 only, with bound check against mapped image span size.

### Safety and Flow Notes

- Metadata builder validates non-zero dimensions/channels/payload and u32 range conversions.
- Frame callback preserves sync publication order: write payload + metadata, then publish ZMQ sync message.
- Control REQ/REP logic was left unchanged.

### Verification

`cmake -B build -S /workspaces/zed-playground/cv-mmap && cmake --build build` passed after changes.

---

## 2026-03-04: Task 5 ZED left+depth packed payload with stable depth fallback

### Implementation learnings

- ZED left image and depth measure can be captured per-frame via `grab()` + `retrieveImage(..., MEM::CPU)` + `retrieveMeasure(MEASURE::DEPTH, MEM::CPU, left_resolution)`.
- For stable two-plane semantics, backend should keep payload shape stable when depth retrieval transiently degrades:
  - if depth retrieve succeeds, cache compact depth bytes as `last_good_depth_plane`
  - if depth retrieve fails/mismatches, reuse cached depth bytes when available, else zero-fill depth payload
- To keep v2 descriptor offsets deterministic, backend now compacts row data into a contiguous packed payload:
  - plane0 LEFT compact bytes then plane1 DEPTH compact bytes
  - `metadata.info.buffer_size` set to total packed payload bytes
- Producer v2 bridge can infer depth plane activation from payload length relative to LEFT compact size and then emit slot1 DEPTH descriptor without changing backend callback ABI.

### Verification

- `cmake -B build -S /workspaces/zed-playground/cv-mmap && cmake --build build` ✅
- `cmake -B build-zed -S /workspaces/zed-playground/cv-mmap -DWITH_BACKEND_ZED=ON && cmake --build build-zed` ✅

---

## 2026-03-04: Task 10 Ethernet-ready Placeholders with Active Stream Path Hardening

### Implementation Summary

Added ethernet-ready placeholder extension points to ZED backend while preserving active `setFromStream` behavior for network modes.

### Files Modified

- `app/backends/app_backends_zed.cpp`: Added extension stub structure and hardened logging

### Ethernet Extension Stub Structure

Added `EthernetExtensionStubs` struct inside `ZedBackendImpl` with commented-out placeholder members:
- `sl::StreamingSender sender` - for future sender mode orchestration
- `sl::StreamingReceiver receiver` - for future receiver coordination  
- `std::jthread discovery_thread` - for future sender discovery
- `std::atomic<bool> sender_ready` - for future health monitoring

Includes `log_placeholder_status()` method that emits DEBUG-level log when network mode is active.

### Stream Path Hardening

Preserved active `setFromStream` path for `network|ethernet|stream` modes with enhanced logging:
- Network mode logs: `[NETWORK MODE: {mode}] opening ZED network stream {ip}:{port}`
- Local mode logs: `[LOCAL MODE: {mode}] opening ZED local camera by {identifier}`
- Extension stub logging triggered only in network path

### Config Guardrails (Preserved from Task 3)

Validation in `app_config.cpp` enforces:
- Network mode requires `ip_address`
- Network mode rejects `serial` and `index` identifiers
- Local mode rejects `ip_address` and `port`
- Mixed identifiers fail at config parse time with clear error messages

### ZED SDK Patterns Observed

From `/usr/local/zed/samples/camera streaming/receiver/cpp/src/main.cpp`:
- Same `setFromStream(ip, port)` pattern used
- Supports IP-only (default port) and IP:port formats
- Passive receiver only - no sender coordination

### Build Verification

Both configurations build successfully:
- Without ZED backend: `cmake -B build -S . && cmake --build build` ✅
- With ZED backend: `cmake -B build-zed -S . -DWITH_BACKEND_ZED=ON && cmake --build build-zed` ✅

### Key Insight

Placeholder approach maintains 100% runtime compatibility while documenting extension points. Stubs are:
- Non-functional (commented out, no active code paths)
- Documented with TODO anchors referencing SDK samples
- Logged for visibility at DEBUG level only
- Zero performance impact

---

## 2026-03-04: Task 7 Python client ABI v1/v2 parser and depth-plane exposure

### Implementation learnings

- A single dispatch function over the 256-byte metadata region (`unmarshal_frame_metadata`) keeps v1 behavior intact while adding v2 parsing with explicit major-version gating.
- v2 safety checks are best enforced at parse time, not at plane-read time:
  - header invariants (`plane_count`, presence mask, descriptor table constants, payload size)
  - per-slot bounds (`offset <= payload_size`, `size <= payload_size - offset`)
  - active/inactive descriptor state rules and deterministic slot ordering (slot0 left, slot1 depth when present).
- Stride-aware ndarray creation must use explicit `shape` and `strides` from descriptor metadata; blind reshape would corrupt padded rows and mixed-depth payloads.
- Keeping backward-compatible metadata aliases (`frame_count`, `timestamp_ns`, `info`) on v2 metadata allows existing iterator consumers to continue working with minimal changes.

### Verification

- `uv run pytest -q` ✅ (7 passed)

---

## 2026-03-04: Task 9 control/sync compatibility policy harmonization

### Implementation learnings

- Control wire compatibility in producer (`src/main.cpp`) remains strict on major `VERSION_MAJOR == 1`; unsupported control majors deterministically return `CONTROL_RESPONSE_INVALID_VERSION`.
- Python parsing had implicit sync-major leniency (`pass`) that made unsupported majors non-deterministic for callers; switching to explicit `ValueError` makes sync/control wire compatibility behavior consistent with producer-side rejection semantics.
- GUI shared-memory client already supports both metadata major v1 and v2 in `SharedBuffer::verify()`; explicit migration-window comment at dispatch site clarifies intended mixed-version state (SHM v2 with sync/control v1) without weakening unsupported-major rejection.

### Verification

- `cmake --build build` in `/workspaces/zed-playground/cv-mmap` ✅
- `.venv/bin/python -m pytest tests/test_import_and_protocol.py` in `/workspaces/zed-playground/cvmmap-python-client` ✅ (7 passed)
- `cv-mmap-gui` build could not be completed in this environment due missing system deps (`OpenSSL` for one cache path and `glfw3` pkg-config in fresh configure), but LSP diagnostics on changed GUI file reported no errors.

---

## 2026-03-04: Task 11 cross-repo runtime interoperability matrix execution

### Evidence artifacts

- Matrix runner script: `/workspaces/zed-playground/cv-mmap/.sisyphus/drafts/run_task11_interop_matrix.sh`
- Run ID: `20260304T201336Z`
- Summary TSV: `/workspaces/zed-playground/cv-mmap/.sisyphus/evidence/task-11-interop-matrix/20260304T201336Z/matrix-summary.tsv`
- Summary JSON: `/workspaces/zed-playground/cv-mmap/.sisyphus/evidence/task-11-interop-matrix/20260304T201336Z/matrix-summary.json`
- Summary Markdown: `/workspaces/zed-playground/cv-mmap/.sisyphus/evidence/task-11-interop-matrix/20260304T201336Z/matrix-summary.md`
- Task evidence pointers:
  - Happy: `/workspaces/zed-playground/cv-mmap/.sisyphus/evidence/task-11-interop-matrix-happy.txt`
  - Error: `/workspaces/zed-playground/cv-mmap/.sisyphus/evidence/task-11-interop-matrix-error.txt`

### Matrix outcomes

- PASS: producer `build` and `build-zed` build variants
- PASS: python parser path (`uv run pytest -q`) and per-fixture checks:
  - v1 valid
  - v2 left-only valid
  - v2 left+depth valid
  - v2 malformed rejection
- PASS: GUI parser fixture checker path (`protocol_fixture_check`)
- BLOCKED: GUI full build due missing system OpenSSL dependency (`Could NOT find OpenSSL ... OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR`)
- FAIL: control reset compatibility cell (`response_code=-6`, producer logs `received control message too small: 34 bytes`)
- FAIL: unsupported-major rejection cell currently receives `response_code=-6` for the same request-size issue before version gate execution

### Key learning

- Current python control request wire format emits 34-byte request headers while producer expects `sizeof(control_message_request_t)==36`; this prevents both reset success and unsupported-major invalid-version path from being exercised via python client in runtime matrix conditions.

---

## 2026-03-04: Task 11 matrix rerun (final evidence set)

- Final run ID: `20260304T201839Z-2762858`
- Final summary: `/workspaces/zed-playground/cv-mmap/.sisyphus/evidence/task-11-interop-matrix/20260304T201839Z-2762858/matrix-summary.tsv`
- Final counts: `cells_total=11`, `cells_pass=10`, `cells_non_pass=1`
- Control compatibility cells now pass with explicit expected responses:
  - reset compatibility: `response_code=0`
  - unsupported-major rejection: `response_code=-5`
- Remaining non-pass cell is environment BLOCKED only:
  - GUI full build blocked by missing OpenSSL (`OPENSSL_CRYPTO_LIBRARY`, `OPENSSL_INCLUDE_DIR`) in `cv-mmap-gui` configure step.

---

## 2026-03-04: Task 10 fixture-driven parser validation across Python + GUI consumers

### Implementation learnings

- Keeping golden protocol fixtures as static `*.hex` assets made both consumers use identical byte-for-byte metadata/payload inputs (v1 valid, v2 left-only valid, v2 left+depth valid, v2 malformed descriptor).
- Python parser tests are now fixture-driven end-to-end for frame metadata decode paths; semantic assertions remained unchanged while input generation moved out of test code.
- For GUI, extracting SHM metadata parsing into a reusable parser unit enabled a lightweight fixture harness that runs without UI/runtime stack and validates explicit malformed out-of-bounds rejection.
- A standalone CMake harness under `app/cvmmap-client/tests` avoids OpenGL/GLFW/OpenCV requirements and is sufficient to enforce parser behavior deterministically.

### Verification

- `uv run pytest -q` in `/workspaces/zed-playground/cvmmap-python-client` ✅ (7 passed)
- `cmake -S app/cvmmap-client/tests -B build-protocol-fixture-check && cmake --build build-protocol-fixture-check && ./build-protocol-fixture-check/protocol_fixture_check` in `/workspaces/zed-playground/cv-mmap-gui` ✅ (`protocol fixture parser checks passed`)

---

## 2026-03-04: Python Control Message ABI Mismatch Fix (Task 11)

### Problem
Python control messages were rejected by cv-mmap server with `CONTROL_RESPONSE_INVALID_MSG_SIZE (-6)`.

C++ validates minimum request size using `sizeof(control_message_request_t) == 36`.

### Root Cause
Python `struct` format strings didn't include trailing padding bytes required for 4-byte alignment.

| Struct | Python (before) | C++ (expected) |
|--------|-----------------|----------------|
| `ControlMessageRequest` | 34 bytes | 36 bytes |
| `ControlMessageResponse` | 38 bytes | 40 bytes |

### Fix
Added explicit padding (`2x`) to marshal format strings in `src/cvmmap/msg.py`:

```python
# Request: was "=BxBBi24sH" (34 bytes), now "=BxBBi24sH2x" (36 bytes)
# Response: was "=BxBBii24sH" (38 bytes), now "=BxBBii24sH2x" (40 bytes)
```

### Files Modified
- `cvmmap-python-client/src/cvmmap/msg.py` - Fixed `marshal_format()` for both classes
- `cvmmap-python-client/tests/test_import_and_protocol.py` - Added explicit header size assertions

### Verification
```bash
$ python3 -m pytest tests/test_import_and_protocol.py -v
test_control_message_header_sizes PASSED
# assert ControlMessageRequest.header_size() == 36
# assert ControlMessageResponse.header_size() == 40
```

---

## 2026-03-04: Task 12 Documentation Sync and Release Sequencing Notes

### Documentation Updated

Files modified to reflect implemented behavior:

| File | Changes |
|------|---------|
| `config_example.toml` | Added ABI v2 rollout notes, ZED validation values/aliases, ethernet stream behavior |
| `docs/abi_v2_migration_guide.md` | New document covering migration policy, rollout sequencing, consumer compatibility |
| `cvmmap-python-client/README.md` | Added ABI v1/v2 support table, migration window notes, rollout sequencing, depth plane usage |
| `cv-mmap-gui/README.md` | Added build requirements, environment notes (OpenSSL), protocol compatibility, parser validation instructions |

### Key Documentation Points

1. **Rollout sequencing is explicit**: Deploy consumers before producer-only-v2 deployments
2. **Strict validation values documented**: Resolution aliases (2k, 1080p, fhd, 720p, hd), depth modes (no aliases)
3. **Unsupported values rejected**: `neural`, `4k`, and other invalid values fail at config parse time
4. **Control wire remains v1**: SHM metadata may be v2 during migration, but control/sync stay v1
5. **GUI build caveat**: Full GUI builds may require OpenSSL (environment dependency, not protocol blocker)
6. **Fixture-driven validation**: Both Python and GUI consumers support deterministic protocol fixture testing

### Files in Scope Verified

- `/workspaces/zed-playground/cv-mmap/config_example.toml`
- `/workspaces/zed-playground/cv-mmap/docs/abi_v2_migration_guide.md` (new)
- `/workspaces/zed-playground/cvmmap-python-client/README.md`
- `/workspaces/zed-playground/cv-mmap-gui/README.md`

---

## 2026-03-04: Task 13 final verification sweep learnings

- Final sign-off run `20260304T202651Z-2774027` confirmed cross-repo minimum gate status: **4 PASS / 0 FAIL / 1 BLOCKED**.
- Producer repo (`cv-mmap`) both required build variants passed cleanly (default and `WITH_BACKEND_ZED=ON`).
- Python client parser/test suite remained green (`uv run pytest -q`: `8 passed`).
- GUI parser fixture checker passed, confirming protocol parser path is healthy in current environment.
- GUI full build is still environment-blocked by OpenSSL discovery (`OPENSSL_CRYPTO_LIBRARY` + `OPENSSL_INCLUDE_DIR` missing), matching previously observed blocker pattern.
- Evidence package for task 13 was captured under `/workspaces/zed-playground/cv-mmap/.sisyphus/evidence/task-13-signoff/20260304T202651Z-2774027` with per-command timestamped logs and sign-off summary.

---

## 2026-03-04: F3 manual QA verification sweep (CLI + Python + GUI fixture)

- `cv-mmap` CLI `--help` renders correctly from current built binary (`./build-zed/cv-mmap --help`).
- Valid config startup path works (`./build-zed/cv-mmap --config config.toml`) and reaches runtime initialization (ZMQ bind + backend open + frame info logs).
- Invalid config path fails fast with explicit error (`./build-zed/cv-mmap --config does_not_exist.toml` → "Config file not found...").
- Python client user-facing test behavior passes in-repo via `uv run pytest -q` (`8 passed`).
- GUI protocol fixture checker executable is runnable and passes (`./build-protocol-fixture-check/protocol_fixture_check`).
- Full GUI app launch remains environment-blocked due OpenSSL dependency resolution failure during CMake configure (`missing: OPENSSL_CRYPTO_LIBRARY OPENSSL_INCLUDE_DIR`).

---

## 2026-03-05: F2 final-review hardening (producer depth gating + GUI parser semantic parity)

- Producer `build_v2_metadata()` no longer infers depth plane from generic `payload > left` heuristic.
- Depth descriptor activation is now gated by explicit packed ZED contract only: payload must equal `left_compact_size + (width * sizeof(float) * height)`.
- When that exact contract is not met, producer stays single-plane (`plane_count=1`, mask `0x01`) and keeps slot1 inactive.
- GUI parser now enforces semantic descriptor minima for every active slot:
  - `stride_bytes >= width * channels(pixel_format) * bytes_per_channel(depth)`
  - `size_bytes >= stride_bytes * height`
- GUI parser rollout guardrails now explicitly reject active slot>=2 and require slot1 depth descriptor shape/type contract (`Depth` plane must be `GRAY/F32`).

---

## 2026-03-05: GUI full build fix - PoseDetection timestamp field mismatch

### Problem

GUI build failure in `cv-mmap-gui/app/main.cpp`: `PoseDetection` has no member `timestamp_unix_ns`.

### Root Cause

The `PoseDetection` struct in `app/pose_protocol/app_pose_protocol.hpp` does not have a `timestamp_unix_ns` field (only has `frame_index`, `reference_size`, bounding boxes, keypoints), but `main.cpp` logging code at lines 370-384 was trying to access `pose.timestamp_unix_ns.has_value()` and `pose.timestamp_unix_ns.value_or(0)`.

### Fix

Removed timestamp-related log arguments from the `spdlog::debug` call in `on_bus_data()`:
- Removed `has_timestamp={}` and `timestamp_unix_ns={}` from format string
- Removed corresponding `pose.timestamp_unix_ns.has_value()` and `pose.timestamp_unix_ns.value_or(0)` arguments

### Verification

```bash
cd /workspaces/zed-playground/cv-mmap-gui
cmake -B build-final-gui -S . && cmake --build build-final-gui
# [100%] Built target cv_mmap_gui ✅
```
