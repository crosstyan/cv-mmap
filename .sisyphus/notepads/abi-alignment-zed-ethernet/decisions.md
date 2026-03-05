
---

## 2026-03-04: ZED Config Validation in app_config.cpp (Task 3 - Config Level)

### Decision: Config-Time vs Backend-Time Validation

Chose to implement validation in `app/config/app_config.cpp` during `Config::from_toml()` rather than deferring to backend open phase.

Rationale:
1. Fail-fast behavior - invalid configs rejected immediately on startup
2. Works even in builds without ZED backend compiled (config validation is backend-agnostic)
3. Centralizes validation logic in one location
4. Prevents partial initialization of resources before detecting invalid config

### Decision: Alias Mapping Strategy

Implemented two-tier mapping for resolution:
1. Explicit aliases (2k → HD2K, 1080p/fhd → HD1080, 720p/hd → HD720) checked first
2. Canonical validation (HD2K, HD1200, HD1080, HD720, SVGA, VGA, AUTO) - case-insensitive

Depth mode has no aliases - only validates canonical values.

### Decision: Case Normalization

All string comparisons use `normalize_ascii_lower()` for case-insensitive matching. Canonical values stored in uppercase.

Benefits:
- User can write "hd1080", "HD1080", "Hd1080" - all accepted
- Backend receives consistent uppercase "HD1080"
- Error messages show allowed values in canonical form

### Decision: Exception Type

Uses `std::invalid_argument` consistent with existing config validation errors in the same file (e.g., `backend_from_string()`, `finite_stream_ending_behavior_from_string()`).

### Files Modified

- `app/config/app_config.cpp`: Added validation functions and integrated into config parsing

### Verification

- Invalid resolution: `./cv-mmap -c invalid.toml` → exits code 1 with clear error
- Invalid depth_mode: `./cv-mmap -c invalid.toml` → exits code 1 with clear error
- Valid aliases: `./cv-mmap -c valid.toml` → config parsed successfully

---

## 2026-03-04: Task 4 Producer ABI v2 Metadata Emission Strategy

### Decision: Keep Producer Write Path v2, Backend Input v1-Compatible

Kept backend callback signatures unchanged (`frame_metadata_t` + single `frame_buffer`) and introduced a producer-side v2 metadata builder in `src/main.cpp`.

Rationale:
1. Meets Task 4 ABI target without requiring backend interface churn.
2. Preserves current backend behavior (OpenCV/GStreamer/ZED) while migrating SHM contract forward.
3. Creates a clear seam for future depth-plane integration by centralizing v2 metadata construction.

### Decision: Current Active Plane Policy = Single LEFT Plane

For current data reality, producer emits:
- `plane_count = 1`
- `plane_presence_mask = 0x01`
- descriptor slot0 = LEFT, slots1-3 zeroed
- `payload_size_bytes = frame_buffer.size()` and payload copy at offset 0

Rationale:
1. Correctly reflects available payload input (single plane) with deterministic ABI-valid metadata.
2. Avoids speculative depth descriptor synthesis from unavailable source frames.
3. Keeps future depth support additive: slot1 can be populated contiguously later.

### Decision: Consistent Frame Identity and Sequence Mapping

Mapped both `frame_id` and `publish_seq` to `metadata.frame_count`, and `capture_ts_ns` to callback timestamp.

Rationale:
1. Maintains monotonicity and traceability with existing sync semantics.
2. Avoids introducing a separate sequence counter in this task.

### Decision: Bounds Safety Before Publish

Implemented validation and bounds checks so payload write occurs only when:
- metadata/payload numeric conversions fit u32,
- dimensions/channels/payload are non-zero,
- destination payload span capacity is sufficient.

Rationale:
1. Prevents SHM overflow and malformed descriptor state.
2. Ensures sync publication happens only after successful metadata+payload commit.

---

## 2026-03-04: Task 5 stable two-plane policy via packed payload + producer split

### Decision: Keep backend callback ABI unchanged, add packed payload transport

Kept existing backend callback contract (`on_frame(std::span<uint8_t>, frame_metadata_t)`) and transported two planes as a single packed byte span:

- payload layout: `[LEFT compact bytes][DEPTH compact bytes]`
- `metadata.info` remains LEFT-plane geometry/depth/pixel_format source
- `metadata.info.buffer_size` updated to total packed payload bytes

Rationale:
1. Avoids backend facade/API churn across OpenCV/GStreamer/ZED.
2. Enables immediate producer v2 slot1 DEPTH descriptor emission from payload arithmetic.
3. Preserves Task 4 sync/control flow and SHM write sequencing.

### Decision: Stable depth policy on transient retrieval degradation

When depth is configured/enabled (`depth_mode != NONE`), backend maintains a stable two-plane payload size every frame:

- Prefer fresh depth bytes when retrieve succeeds.
- On transient retrieve/type/shape degradation, reuse last good depth bytes.
- If no cached depth exists yet, zero-fill depth bytes with same plane size.

Rationale:
1. Prevents per-frame plane-count oscillation due transient depth failures.
2. Keeps producer descriptor policy deterministic (`plane_count=2`, mask `0x03`) whenever extra depth bytes are validly present.
3. Preserves stream continuity/reconnect behavior without introducing control-path changes.

---

## 2026-03-04: Task 10 Ethernet-ready Placeholders - Implementation Decisions

### Decision 1: Placeholder Approach - Commented Members vs Active Stubs

**Decision**: Use commented-out member declarations with TODO anchors.

**Rationale**:
- Avoids compiling unused code
- Documents intended extension points clearly
- Maintains exact runtime semantics today
- Easy to activate when implementing sender/receiver coordination

**Alternative Rejected**: Active stub methods - would add no-ops to runtime

### Decision 2: Extension Stub Logging

**Decision**: Add `log_placeholder_status()` that emits at DEBUG level when network mode is active.

**Rationale**:
- Provides visibility that extension points exist
- Zero cost at INFO level (default) or higher
- Only triggered in network path (relevant context)

### Decision 3: Hardened Log Messages with Mode Tags

**Decision**: Prefix log messages with `[NETWORK MODE: X]` and `[LOCAL MODE: X]` tags.

**Rationale**:
- Makes stream mode selection explicit in logs
- Aids debugging configuration issues
- Distinguishes between multiple mode aliases (network/ethernet/stream)
- No runtime cost (logs emitted once at open)

### Decision 4: No Sender/Receiver State Yet

**Decision**: Do not add actual `sl::StreamingSender` or `sl::StreamingReceiver` members yet.

**Rationale**:
- Would require constructor/destructor changes
- May affect move/copy semantics of backend
- Better to implement when sender coordination is actually needed
- Current focus is receiver-only (consumer) mode

### Decision 5: Preserve Existing Config Validation

**Decision**: No changes to `app_config.cpp` - Task 3 validation is complete and correct.

**Rationale**:
- Fail-fast at config parse time is correct behavior
- Mixed local/network identifiers rejected early
- Canonical normalization simplifies backend logic

### Future Work

When implementing full sender/receiver coordination:
1. Uncomment and activate `StreamingSender` member
2. Add sender lifecycle management (separate thread)
3. Implement sender discovery mechanism
4. Add connection health monitoring
5. Consider dynamic quality adaptation

Reference: `/usr/local/zed/samples/camera streaming/sender/` for sender patterns

---

## 2026-03-04: Task 7 Python client ABI v1/v2 parser decisions

### Decision 1: Keep v1 constants unchanged; parse metadata major independently

**Decision**: Kept protocol constants (`VERSION_MAJOR = 1`) untouched for sync/control compatibility and added explicit metadata major constants (`FRAME_METADATA_V1_MAJOR`, `FRAME_METADATA_V2_MAJOR`) for SHM parsing.

**Rationale**:
- Avoids accidental behavior changes in existing ZMQ message marshal/unmarshal paths.
- Makes metadata ABI dispatch explicit and future-proof.

### Decision 2: Parse-time hard rejection for malformed v2 descriptors

**Decision**: Validate v2 header and descriptor invariants during `FrameMetadataV2.unmarshal()` and raise `ValueError` immediately on violations.

**Rationale**:
- Prevents downstream consumers from operating on invalid payload slices.
- Gives deterministic, clear failure points for out-of-bounds and ordering errors.

### Decision 3: Backward-compatible `__aiter__` semantics with v2 left-plane default

**Decision**: `CvMmapClient.__aiter__` still yields `(image, metadata)` where `image` is left-plane data; for v2 this uses descriptor-driven, stride-aware left-plane view.

**Rationale**:
- Preserves caller expectations for current iteration usage.
- Adds depth access via explicit APIs (`client.depth_plane(metadata)` / `metadata.depth_plane(payload)`) without breaking existing code.

### Decision 4: Expose depth plane as optional, deterministic slot-based accessor

**Decision**: Exposed slot0/slot1 accessors with deterministic policy and `None` for missing depth.

**Rationale**:
- Matches producer convention and ksy invariants (slot0 left, slot1 depth when present).
- Keeps client logic simple for both left-only and left+depth streams.

---

## 2026-03-04: Task 9 migration-window version-policy decisions

### Decision 1: Keep control-wire major strict at v1 on producer

**Decision**: Preserve `src/main.cpp` control request gate `req->versions_major == VERSION_MAJOR` and keep deterministic rejection via `CONTROL_RESPONSE_INVALID_VERSION`.

**Rationale**:
- Migration scope is SHM metadata compatibility, not control protocol migration.
- Prevents accidental acceptance of unknown control ABI majors.

### Decision 2: Make python sync/control major checks explicit and deterministic

**Decision**: In `cvmmap/msg.py`, reject unsupported sync and control-response majors with `ValueError` instead of silently continuing.

**Rationale**:
- Aligns python behavior with producer deterministic major-version rejection philosophy.
- Keeps minor-version leniency intact for migration stability.

### Decision 3: Document mixed-version acceptance boundary in GUI SHM path

**Decision**: Keep GUI shared-memory metadata verification accepting v1/v2 majors only, and add explicit inline migration-policy comment at v2 dispatch.

**Rationale**:
- Makes the intended migration window explicit in code where compatibility is enforced.
- Preserves strict failure for unsupported SHM metadata majors.

---

## 2026-03-04: Task 11 runtime interoperability matrix decisions

### Decision 1: Preserve non-pass truth in matrix summary (PASS/FAIL/BLOCKED)

**Decision**: Matrix runner records strict per-cell outcomes without normalization to all-pass.

**Rationale**:
- Required deliverable is verifiable interop evidence, not optimistic reporting.
- Environment limitations and protocol mismatches must remain explicit in machine-readable output.

### Decision 2: Treat GUI full-build dependency failure as BLOCKED, not FAIL

**Decision**: Classify missing system dependency errors (e.g., OpenSSL not found) as `BLOCKED` with concrete excerpt.

**Rationale**:
- This is an environment provisioning issue rather than parser/harness regression.
- Keeps failure taxonomy meaningful across runtime and infra dimensions.

### Decision 3: Keep control compatibility cells as FAIL with concrete producer evidence

**Decision**: Mark control reset and unsupported-major cells as `FAIL` because runtime response was `-6` (invalid message size) instead of expected `OK` / `INVALID_VERSION`.

**Rationale**:
- Producer logs show request-size rejection (`received control message too small: 34 bytes`) in both cells.
- This is a real protocol interoperability gap in runtime path and must not be masked as blocked.

### Decision 4: Add deterministic matrix runner under cv-mmap evidence workflow

**Decision**: Added single script at `.sisyphus/drafts/run_task11_interop_matrix.sh` that executes required cells, stores per-cell logs, and emits TSV/JSON/Markdown summary files.

**Rationale**:
- Ensures reproducibility and appendable evidence generation for follow-up runs.
- Centralizes cross-repo command orchestration without adding dependencies.

### Decision 5: Finalize control cells with explicit 36-byte unsupported-major request packing

**Decision**: Keep reset compatibility via existing python request client and send unsupported-major probe using an explicitly padded 36-byte request layout (`=BxBBi24sHxx`) in matrix harness.

**Rationale**:
- Producer requires `sizeof(control_message_request_t)==36`; unpadded 34-byte requests are rejected before version checks.
- This allows deterministic validation of the unsupported-major rejection requirement (`CONTROL_RESPONSE_INVALID_VERSION = -5`) in runtime matrix evidence.

### Decision 6: Use latest unique run-id artifacts as canonical Task-11 evidence

**Decision**: Treat run `20260304T201839Z-2762858` as the canonical matrix output and retain earlier runs as historical debugging traces.

**Rationale**:
- Latest run has complete required cells with stable outcomes and no duplicate-row collision.
- Canonical set cleanly reports one environment BLOCKED cell (GUI OpenSSL dependency) and all other required cells passing.

---

## 2026-03-04: Task 10 fixture-suite completion decisions

### Decision 1: Use shared deterministic hex fixtures in both repos

**Decision**: Keep canonical fixture assets as static hex files under each repo’s version-controlled test fixture directory.

**Rationale**:
- Ensures reproducible parser inputs with no runtime randomness.
- Keeps malformed-case bytes explicit and reviewable.
- Allows Python and GUI validations to assert against the same binary contract shape.

### Decision 2: Python tests consume fixture files, not inline metadata builders

**Decision**: Rework parser tests to load metadata/payload fixtures from `tests/fixtures/protocol/*.hex` and keep existing assertions.

**Rationale**:
- Completes Task 10 requirement for file-driven golden tests.
- Preserves semantic coverage while eliminating inline fixture construction drift risk.

### Decision 3: Add parser-only GUI harness with isolated CMake entrypoint

**Decision**: Introduce `app_cvmmap_parser` extraction plus `app/cvmmap-client/tests/protocol_fixture_check.cpp` built via its own CMake project.

**Rationale**:
- Enables fixture validation in environments lacking GUI/runtime dependencies.
- Reuses production parser logic instead of duplicating checks in ad-hoc test code.
- Validates required acceptance/rejection cases, including explicit malformed out-of-bounds failure.

---

## 2026-03-04: Add Trailing Padding to Python Control Message Formats

**Decision:** Add `2x` padding bytes to Python struct format strings to match C++ `sizeof()` values.

**Context:**
- C++ structs `control_message_request_t` and `control_message_response_t` end with `uint16_t` length field
- C++ compiler adds 2 bytes trailing padding for 4-byte alignment (largest member is `int32_t`)
- Python's `struct` module doesn't auto-add padding; must be explicit

**Format Changes:**
```
Request:  "=BxBBi{LABEL_LEN_MAX}sH"   → "=BxBBi{LABEL_LEN_MAX}sH2x"   (34→36 bytes)
Response: "=BxBBii{LABEL_LEN_MAX}sH"  → "=BxBBii{LABEL_LEN_MAX}sH2x"  (38→40 bytes)
```

**Test Added:**
```python
def test_control_message_header_sizes():
    assert ControlMessageRequest.header_size() == 36
    assert ControlMessageResponse.header_size() == 40
```

**Rationale:**
- Minimal change: only adds padding, preserves all semantics
- Explicit is better than implicit: `2x` makes padding visible
- Test assertions prevent future regressions
- Matches C++ ABI exactly for wire compatibility

---

## 2026-03-04: Task 12 Documentation Hardening and Release Sequencing Decisions

### Decision 1: Centralize migration policy in dedicated guide document

**Decision:** Create `docs/abi_v2_migration_guide.md` as the canonical source for migration policy, rollout sequencing, and consumer compatibility matrix.

**Rationale:**
- Single source of truth prevents drift between config examples, READMEs, and implementation
- Rollout sequencing is safety-critical and must be explicit and discoverable
- Version policy belongs in documentation, not just code comments

### Decision 2: Document strict parser policy explicitly with valid values and aliases

**Decision:** Update `config_example.toml` with comprehensive comments showing:
- All valid canonical values for resolution and depth_mode
- All accepted aliases (2k, 1080p, fhd, 720p, hd)
- Explicit note that invalid values like `neural`, `4k` are rejected

**Rationale:**
- Users need to know what values work without reading source code
- Prevents confusion when strict validation rejects commonly-attempted but unsupported values
- Documentation must match implemented behavior exactly

### Decision 3: Include environment caveat for GUI full builds

**Decision:** Document in `cv-mmap-gui/README.md` that full GUI builds may require system dependencies like OpenSSL, and classify this as an environment issue rather than a protocol blocker.

**Rationale:**
- Matrix evidence showed GUI full build BLOCKED due to OpenSSL in this environment
- Parser-only builds work without these dependencies
- Users should understand the distinction between protocol compatibility and build environment requirements

### Decision 4: Cross-link documentation across repositories

**Decision:** Reference the producer's migration guide from Python client and GUI READMEs.

---

## 2026-03-05: F2 depth-plane inference and GUI semantic validation decisions

### Decision 1: Producer depth slot activation must be exact-contract only

**Decision**: In `src/main.cpp`, activate slot1 depth descriptor only when payload matches exact packed contract:
`payload_size == left_compact_size + (width * sizeof(float) * height)`.

**Rationale**:
- Prevents false-positive depth-plane metadata from non-ZED or padded payloads.
- Aligns slot1 enablement with known ZED packing semantics already used in backend path.
- Keeps fallback deterministic and safe (single-plane metadata when contract is not exact).

### Decision 2: GUI parser parity with semantic minima and rollout slot-domain checks

**Decision**: In `app_cvmmap_parser.cpp`, enforce active-descriptor semantic checks matching consumer safety expectations:
- minimum stride from `pixel_format + depth`
- minimum size from `stride * height`
- active slot-domain restrictions for current rollout (slot0 LEFT, slot1 DEPTH only)
- slot1 descriptor contract constrained to `GRAY/F32`.

**Rationale**:
- Prevents accepting structurally in-bounds but semantically invalid descriptors.
- Aligns GUI parser behavior with stricter multi-plane contract validation policy.
- Explicitly fences unsupported active slots (2/3) until protocol rollout expands.

**Rationale:**
- Consumers need to understand producer-side version policy
- Prevents duplicated documentation that can drift out of sync
- Establishes clear hierarchy: producer guide is authoritative

### Decision 5: Document fixture-driven validation availability

**Decision:** Include instructions for running protocol fixture checks in both Python client and GUI documentation.

**Rationale:**
- Fixture-driven validation is a key verification mechanism
- Users should be able to validate their consumer implementation against golden fixtures
- Separates parser testing from full GUI runtime requirements

---

## 2026-03-04: Task 13 final sign-off packaging decisions

### Decision 1: Classify GUI full-build OpenSSL issue as BLOCKED (not FAIL)

**Decision:** Mark GUI full build as **BLOCKED** in final status table when configure fails on missing OpenSSL development environment variables/libraries.

**Rationale:**
- Failure originates from environment dependency provisioning (`FindOpenSSL`) rather than protocol/parser logic.
- GUI parser fixture checker passed in the same sweep, isolating protocol path health from full dependency chain availability.

### Decision 2: Use per-command timestamped logs + single summary index as sign-off contract

**Decision:** Store one timestamped log per verification command and link all logs from a single `signoff-summary.md`, plus a machine-readable `verification-status.tsv`.

**Rationale:**
- Maintains traceability and auditability for each gate command.
- Supports both human review and scripted downstream consumption.

### Decision 3: No-code-change rollback policy for Task 13

**Decision:** Record rollback as not applicable for this task because only evidence/notepad artifacts were changed.

**Rationale:**
- Task 13 is a verification-and-packaging quality gate.
- Mitigation focuses on dependency remediation (OpenSSL provisioning) and re-run, not source rollback.
