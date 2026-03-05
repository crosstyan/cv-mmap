# ABI v2 Alignment Across cv-mmap, Python Client, and GUI + ZED Ethernet Placeholder Completion

## TL;DR
> **Summary**: Implement ABI v2 multi-plane metadata/payload support in `cv-mmap` and align both consumers (`cvmmap-python-client`, `cv-mmap-gui`) to parse and expose left+depth planes, while preserving rollout safety via explicit v1/v2 handling and keeping active ZED network stream support.
> **Deliverables**:
> - Producer ABI v2 implementation aligned with `docs/cvmmap.ksy`
> - Python client dual-version parser + depth exposure
> - GUI dual-version parser + depth exposure path
> - ZED config/parser strictness hard-fail policy
> - Task 10 ethernet-ready hardening with active `setFromStream`
> - Cross-repo verification matrix and evidence artifacts
> **Effort**: XL
> **Parallel**: YES - 4 waves
> **Critical Path**: 1 → 2 → 4 → 7 → 10 → 13

## Context
### Original Request
- Align new ABI from `cv-mmap/.sisyphus/plans/zed-backend-depth-plane.md` to:
  - `/workspaces/zed-playground/cvmmap-python-client`
  - `/workspaces/zed-playground/cv-mmap-gui`
- Implement Task 10 Ethernet-ready placeholders, following ZED SDK references in `/usr/local/zed` and `/workspaces/zed-playground/playground/zed-sdk`.

### Interview Summary
- Decision: keep rollout v1-compatible (no hard major-bump cutover requirement).
- Decision: keep current active network stream path (`setFromStream`) for Task 10; do not downgrade to comments-only placeholders.
- Decision: expose depth in both Python and GUI in this iteration.
- Decision: fail fast for unknown/unsupported ZED resolution/depth_mode values.

### Metis Review (gaps addressed)
- Addressed: sequencing risk (consumers must be v2-capable before producer-only-v2 deployments).
- Addressed: config mismatch risk (`1080p`/`neural` in config example vs parser behavior).
- Addressed: v2 invariants risk by making `docs/cvmmap.ksy` normative source.
- Addressed: edge cases for stride, remap on payload changes, depth transient behavior, and control/version compatibility.

## Work Objectives
### Core Objective
Deliver a decision-complete, executable cross-repo migration to ABI v2 multi-plane metadata and payload semantics with depth exposure in both consumers, without regressing existing runtime behavior (including active ZED network stream mode).

### Deliverables
- `cv-mmap` producer emits ABI v2 metadata and two-plane payload (left + depth), per ksy invariants.
- `cvmmap-python-client` parses v1/v2 metadata and returns left and depth data with correct dtype/shape/stride.
- `cv-mmap-gui` parses v1/v2 metadata and exposes depth plane in rendering/application path.
- ZED config/parser paths enforce strict validation and preserve active network stream open behavior.
- Automated verification artifacts across all three repos.

### Definition of Done (verifiable conditions with commands)
- `cv-mmap` builds with ZED OFF and ON:
  - `cmake -B build -S . && cmake --build build`
  - `cmake -B build-zed -S . -DWITH_BACKEND_ZED=ON && cmake --build build-zed`
- Python client tests pass:
  - `python -m pytest -q`
- GUI builds:
  - `cmake -B build -S . && cmake --build build`
- Protocol tests pass for v1 and v2 golden fixtures (Python + GUI parser tests).
- Runtime smoke confirms frame_count monotonic, valid plane descriptors, left/depth readable.

### Must Have
- `docs/cvmmap.ksy` treated as ABI source of truth.
- v2 metadata header and descriptors follow exact field sizes/offsets/invariants.
- Depth plane exported and consumable in both Python and GUI.
- Active ZED network stream path retained and validated.
- Strict fail-fast for invalid ZED mode/resolution/depth mode config.

### Must NOT Have (guardrails, AI slop patterns, scope boundaries)
- No TCP transport redesign for consumers.
- No speculative protocol redesign outside v1/v2 compatibility scope.
- No placeholder comments replacing active stream functionality.
- No silent fallback for invalid ZED config values.

## Verification Strategy
> ZERO HUMAN INTERVENTION — all verification is agent-executed.
- Test decision: tests-after + existing frameworks (CMake build checks, pytest, targeted parser/unit tests, runtime smoke scripts)
- QA policy: Every task includes happy + failure/edge scenarios.
- Evidence: `.sisyphus/evidence/task-{N}-{slug}.{ext}`

## Execution Strategy
### Parallel Execution Waves
> Target: 5-8 tasks per wave. <3 per wave (except final) = under-splitting.
> Extract shared dependencies as Wave-1 tasks for max parallelism.

Wave 1: ABI contract finalization + guardrails + strict config policy
Wave 2: Producer ABI v2 + ZED backend metadata/payload integration
Wave 3: Python and GUI dual-version consumers + depth exposure
Wave 4: Cross-repo integration tests + rollout evidence

### Dependency Matrix (full, all tasks)
- 1 blocks 2,3,4,5,6,7,8,9,10,11
- 2 blocks 4,5,6
- 3 blocks 5,6
- 4 blocks 10,11
- 5 blocks 10,11
- 6 blocks 10,11
- 7 blocks 10,11
- 8 blocks 10,11
- 9 blocks 10,11
- 10 blocks 12
- 11 blocks 12
- 12 blocks 13

### Agent Dispatch Summary (wave → task count → categories)
- Wave 1 → 3 tasks → deep / unspecified-high
- Wave 2 → 4 tasks → deep / unspecified-high / quick
- Wave 3 → 4 tasks → deep / unspecified-high
- Wave 4 → 2 tasks → unspecified-high / deep

## TODOs
> Implementation + Test = ONE task. Never separate.
> EVERY task MUST have: Agent Profile + Parallelization + QA Scenarios.

<!-- TASKS_INSERT_BEFORE_FINAL_VERIFICATION -->

- [x] 1. Lock ABI v2 contract as implementation source of truth

  **What to do**: Align concrete implementation targets to `docs/cvmmap.ksy` v2 definitions (header size, descriptor size/capacity, offsets, masks, ordering) and create a contract checklist file under each repo’s tests/docs area used by implementers to avoid drift.
  **Must NOT do**: Do not invent field names/layouts that differ from ksy; do not alter ksy semantics in this task.

  **Recommended Agent Profile**:
  - Category: `deep` — Reason: Cross-repo ABI invariants require exactness.
  - Skills: `[]` — No special skill required.
  - Omitted: `playwright` — Not UI/browser work.

  **Parallelization**: Can Parallel: NO | Wave 1 | Blocks: [2,3,4,5,6,7,8,9,10,11] | Blocked By: []

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/docs/cvmmap.ksy:314-404` — v2 header/descriptors/padding layout
  - Pattern: `cv-mmap/docs/cvmmap.ksy:384-390` — deterministic plane ordering
  - Pattern: `cv-mmap/app/models/app_metadata_models.hpp:132-208` — current v1 structs
  - Pattern: `cv-mmap/app/models/app_common_models.hpp:35-36` — current version constants

  **Acceptance Criteria** (agent-executable only):
  - [ ] Contract checklist artifact exists and maps every v2 field to target code location.
  - [ ] All listed offsets/sizes match ksy values exactly.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Contract checklist completeness
    Tool: Bash
    Steps: Run `grep -R "plane_descriptor_size\|plane_descriptor_capacity\|plane_presence_mask" /workspaces/zed-playground/cv-mmap/docs/cvmmap.ksy`
    Expected: All normative fields present and captured in checklist artifact.
    Evidence: .sisyphus/evidence/task-1-abi-contract.txt

  Scenario: Contract mismatch guard
    Tool: Bash
    Steps: Run `grep -R "versions_major\|plane_count" /workspaces/zed-playground/cv-mmap/docs/cvmmap.ksy`
    Expected: Checklist references exact lines; any missing mapping is flagged as failure.
    Evidence: .sisyphus/evidence/task-1-abi-contract-error.txt
  ```

  **Commit**: YES | Message: `docs(protocol): lock abi v2 contract checklist` | Files: [`cv-mmap/docs/*`, `cvmmap-python-client/tests/*`, `cv-mmap-gui/app/*tests*`]

- [x] 2. Implement C++ ABI v2 metadata/descriptor models in cv-mmap

  **What to do**: Add ABI v2 C++ structs (packed/fixed-size semantics matching ksy), including v2 header and 4 descriptor slots, plus conversion helpers from v1-compatible frame info to v2 plane-0 and depth plane descriptors.
  **Must NOT do**: Do not remove existing v1 sync/control message structs; do not change `SHM_PAYLOAD_OFFSET` from 256.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` — Reason: low-level struct layout + ABI safety.
  - Skills: `[]` — no extra skill needed.
  - Omitted: `frontend-ui-ux` — backend/protocol work.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: [4,5,10,11] | Blocked By: [1]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/docs/cvmmap.ksy:278-369` — descriptor/header definitions
  - Pattern: `cv-mmap/app/models/app_metadata_models.hpp:132-208` — existing metadata patterns/static asserts
  - API/Type: `cv-mmap/app/models/app_enum_models.hpp` — `Depth`, `PixelFormat`, `FramePlaneType`
  - Pattern: `cv-mmap/app/models/app_models.cpp:52-69` — depth size helper semantics

  **Acceptance Criteria** (agent-executable only):
  - [ ] `cv-mmap` builds successfully after model additions.
  - [ ] `sizeof`/`alignof` static assertions enforce expected ABI v2 sizes.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Build validates struct assertions
    Tool: Bash
    Steps: Run `cmake -B build -S /workspaces/zed-playground/cv-mmap && cmake --build build`
    Expected: Build succeeds; no static_assert failures for new v2 types.
    Evidence: .sisyphus/evidence/task-2-v2-model-build.txt

  Scenario: Negative compile check on intentional mismatch
    Tool: Bash
    Steps: Run configured unit/static checks that compare declared constants with expected 64/24/4 values.
    Expected: Any mismatch fails checks with explicit message.
    Evidence: .sisyphus/evidence/task-2-v2-model-error.txt
  ```

  **Commit**: YES | Message: `feat(protocol): add abi v2 metadata and plane descriptors` | Files: [`cv-mmap/app/models/*`]

- [x] 3. Enforce strict ZED config validation and canonical value mapping

  **What to do**: Update config parsing and ZED backend parser to support canonical aliases (`1080p`→`HD1080`, `2k`→`HD2K`, `4k`→supported nearest explicit value), explicitly support/validate depth mode options including `neural` where SDK supports it, and fail-fast on unknown values.
  **Must NOT do**: Do not keep warn+fallback behavior for unknown resolution/depth mode.

  **Recommended Agent Profile**:
  - Category: `quick` — Reason: concentrated parser validation changes.
  - Skills: `[]`
  - Omitted: `playwright` — not UI.

  **Parallelization**: Can Parallel: YES | Wave 1 | Blocks: [5,10,12] | Blocked By: [1]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/app/backends/app_backends_zed.cpp:52-97` — current fallback parse behavior
  - Pattern: `cv-mmap/app/config/app_config.cpp:200-344` — current zed config validation
  - Pattern: `cv-mmap/config_example.toml:14-30` — current documented values (`1080p`, `neural`)
  - External: `/usr/local/zed/samples/camera streaming/single_sender/cpp/src/main.cpp:123-140` — accepted resolution labels in samples

  **Acceptance Criteria** (agent-executable only):
  - [ ] Invalid `zed.resolution` fails parsing/open with explicit error.
  - [ ] Invalid `zed.depth_mode` fails parsing/open with explicit error.
  - [ ] Config example values parse without fallback warnings.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Valid config parsing
    Tool: Bash
    Steps: Run cv-mmap config load path using config_example.toml with zed section.
    Expected: No fallback warning; parsed values map to valid SDK enums.
    Evidence: .sisyphus/evidence/task-3-zed-config-valid.txt

  Scenario: Invalid value rejection
    Tool: Bash
    Steps: Use a temp TOML with `resolution="invalid"` and run app startup/config parse.
    Expected: Process exits with explicit invalid_argument message.
    Evidence: .sisyphus/evidence/task-3-zed-config-error.txt
  ```

  **Commit**: YES | Message: `fix(config): fail fast on invalid zed mode and resolution` | Files: [`cv-mmap/app/config/*`, `cv-mmap/app/backends/app_backends_zed.cpp`, `cv-mmap/config_example.toml`]

- [x] 4. Upgrade producer SHM write path to ABI v2 metadata + multi-plane payload packing

  **What to do**: Refactor `src/main.cpp` producer write path to allocate payload for v2 plane packing (left at offset 0, depth contiguous after left), write v2 metadata region [0..255], and publish sync after metadata+payload commit.
  **Must NOT do**: Do not break control REQ/REP protocol behavior; do not move payload offset from 256.

  **Recommended Agent Profile**:
  - Category: `deep` — Reason: central dataflow and ABI safety.
  - Skills: `[]`
  - Omitted: `frontend-ui-ux`.

  **Parallelization**: Can Parallel: NO | Wave 2 | Blocks: [10,11,12] | Blocked By: [1,2]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/src/main.cpp:398-444` — current single-plane metadata/frame copy flow
  - Pattern: `cv-mmap/src/main.cpp:301-341` — frame_state + SHM spans
  - Pattern: `cv-mmap/docs/cvmmap.ksy:375-404` — full v2 metadata region layout
  - Pattern: `cv-mmap/app/models/app_common_models.hpp:12` — fixed payload offset

  **Acceptance Criteria** (agent-executable only):
  - [ ] Producer writes valid v2 header and descriptor fields for active planes.
  - [ ] Payload order and descriptor offsets are contiguous and in-bounds.
  - [ ] Sync publication remains functional.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: v2 metadata layout verification
    Tool: Bash
    Steps: Run producer and capture SHM bytes; validate offsets and header values against ksy invariants.
    Expected: plane_descriptors_offset=64, descriptor_size=24, capacity=4, plane_count valid.
    Evidence: .sisyphus/evidence/task-4-producer-v2-layout.txt

  Scenario: Descriptor bounds failure
    Tool: Bash
    Steps: Run parser/check script against intentionally malformed fixture with invalid offsets.
    Expected: Parser/check rejects metadata as invalid.
    Evidence: .sisyphus/evidence/task-4-producer-v2-error.txt
  ```

  **Commit**: YES | Message: `feat(producer): emit abi v2 metadata and packed planes` | Files: [`cv-mmap/src/main.cpp`, `cv-mmap/app/models/*`]

- [x] 5. Implement ZED backend depth plane capture and stable two-plane metadata policy

  **What to do**: Extend ZED backend frame capture to retrieve left image and depth measure, produce plane descriptors for both, and adopt stable plane policy (two planes when depth enabled; invalid depth values represented in-plane when retrieval degrades rather than dynamic plane count oscillation unless explicitly forced by startup failure).
  **Must NOT do**: Do not regress local camera open/reconnect or active network `setFromStream` path.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` — Reason: SDK integration + runtime failure handling.
  - Skills: `[]`
  - Omitted: `frontend-ui-ux`.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: [10,11,12] | Blocked By: [1,2,3]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/app/backends/app_backends_zed.cpp:201-333` — open/capture/frame-size path
  - Pattern: `cv-mmap/app/backends/app_backends_zed.cpp:371-395` — metadata + frame callback flow
  - External: `/usr/local/zed/samples/camera streaming/receiver/cpp/src/main.cpp:166-177` — grab/retrieveImage pattern
  - External: `/usr/local/zed/include/sl/Camera.hpp` — `retrieveMeasure`, depth mat types, runtime params

  **Acceptance Criteria** (agent-executable only):
  - [ ] Left + depth captured in CPU memory and mapped into v2 descriptors.
  - [ ] Stream mode local/network still opens and captures frames.
  - [ ] Reconnect loop handles depth retrieval failures gracefully.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Happy path left+depth
    Tool: Bash
    Steps: Run with ZED backend config and log emitted descriptor metadata for first N frames.
    Expected: plane_count=2, slot0=left, slot1=depth, offsets contiguous.
    Evidence: .sisyphus/evidence/task-5-zed-depth-happy.txt

  Scenario: Depth retrieval degraded
    Tool: Bash
    Steps: Trigger/ simulate depth retrieval failure and continue capture loop.
    Expected: Stream remains alive; failure is logged; metadata policy remains stable per design.
    Evidence: .sisyphus/evidence/task-5-zed-depth-error.txt
  ```

  **Commit**: YES | Message: `feat(zed): add depth plane capture and descriptor emission` | Files: [`cv-mmap/app/backends/app_backends_zed.cpp`, `cv-mmap/app/backends/app_backends_zed.hpp`]

- [x] 6. Complete Task 10 Ethernet-ready placeholders with active stream path hardening

  **What to do**: Keep active `setFromStream` runtime behavior and add explicit placeholder extension points for future multi-cam ethernet sender/receiver orchestration (documented stubs, TODO anchors, and validation hooks). Enforce stream-mode constraints (ip required, serial/index forbidden in network mode).
  **Must NOT do**: Do not convert network support into comments-only or dead code.

  **Recommended Agent Profile**:
  - Category: `quick` — Reason: mostly guardrail/docs and targeted runtime hardening.
  - Skills: `[]`
  - Omitted: `playwright`.

  **Parallelization**: Can Parallel: YES | Wave 2 | Blocks: [12] | Blocked By: [1,3]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/app/backends/app_backends_zed.cpp:207-231` — existing network/local branch
  - Pattern: `cv-mmap/app/config/app_config.cpp:326-340` — network mode constraints
  - External: `/usr/local/zed/samples/camera streaming/receiver/cpp/src/main.cpp:103-110` — stream init API
  - External: `/usr/local/zed/samples/camera streaming/single_sender/cpp/src/main.cpp:63-83` — sender-side streaming lifecycle for future placeholders

  **Acceptance Criteria** (agent-executable only):
  - [ ] `stream_mode=network|ethernet|stream` path still calls `setFromStream`.
  - [ ] Misconfigured network/local mixed identifiers fail clearly.
  - [ ] Placeholder extension points are present and documented.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Active network stream call path
    Tool: Bash
    Steps: Start app with network stream config and inspect logs/call traces.
    Expected: setFromStream called with ip[:port] and camera open attempted.
    Evidence: .sisyphus/evidence/task-6-ethernet-placeholder-happy.txt

  Scenario: Invalid network config
    Tool: Bash
    Steps: Provide network mode plus serial/index and run startup.
    Expected: startup rejected with explicit invalid_argument.
    Evidence: .sisyphus/evidence/task-6-ethernet-placeholder-error.txt
  ```

  **Commit**: YES | Message: `chore(zed): harden ethernet placeholders while keeping stream active` | Files: [`cv-mmap/app/backends/app_backends_zed.cpp`, `cv-mmap/app/config/app_config.cpp`, `cv-mmap/config_example.toml`]

- [x] 7. Add Python client ABI v1/v2 parser and depth plane API exposure

  **What to do**: Extend Python protocol models to decode v2 header+descriptors while preserving v1 parsing. Update client memory mapping to support multi-plane payload slices, stride-aware ndarray views, and explicit depth plane accessors in public API.
  **Must NOT do**: Do not keep implicit assumption that payload is always single `uint8 (H,W,C)` plane.

  **Recommended Agent Profile**:
  - Category: `deep` — Reason: binary parsing + ndarray stride correctness.
  - Skills: `[]`
  - Omitted: `frontend-ui-ux`.

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: [10,11,12] | Blocked By: [1,4]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cvmmap-python-client/src/cvmmap/msg.py:169-280` — existing v1 frame metadata parsing
  - Pattern: `cvmmap-python-client/src/cvmmap/__init__.py:136-257` — shared memory view creation and iteration
  - Pattern: `cv-mmap/docs/cvmmap.ksy:314-404` — v2 metadata schema
  - Test: `cvmmap-python-client/tests/test_import_and_protocol.py:26-31` — current struct-size baseline

  **Acceptance Criteria** (agent-executable only):
  - [ ] Python parser handles v1 and v2 metadata fixtures.
  - [ ] Public API provides left and depth plane data for v2 streams.
  - [ ] v1 behavior remains unchanged for existing clients.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: v2 parse and depth exposure
    Tool: Bash
    Steps: Run pytest cases with v2 golden metadata+payload fixtures.
    Expected: left plane ndarray and depth ndarray returned with expected dtype/shape/stride.
    Evidence: .sisyphus/evidence/task-7-python-v2-happy.txt

  Scenario: malformed v2 descriptor rejection
    Tool: Bash
    Steps: Run pytest on fixture with invalid offset/size bounds.
    Expected: parser raises explicit ValueError/RuntimeError.
    Evidence: .sisyphus/evidence/task-7-python-v2-error.txt
  ```

  **Commit**: YES | Message: `feat(py-client): add abi v2 parsing and depth plane exposure` | Files: [`cvmmap-python-client/src/cvmmap/msg.py`, `cvmmap-python-client/src/cvmmap/__init__.py`, `cvmmap-python-client/tests/*`]

- [x] 8. Add GUI client ABI v1/v2 parser and depth plane exposure path

  **What to do**: Update GUI cvmmap client structs/parser for dual-version metadata, payload slicing by plane descriptors, and expose depth plane into render/application context (minimum: available for processing/view path with clear compatibility behavior for v1).
  **Must NOT do**: Do not break existing plane-0 texture render loop.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` — Reason: shared-memory parser + rendering integration.
  - Skills: `[]`
  - Omitted: `playwright` — desktop GUI, not browser.

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: [10,11,12] | Blocked By: [1,4]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap-gui/app/cvmmap-client/app_cvmmap_client.hpp:105-146` — current v1 layout
  - Pattern: `cv-mmap-gui/app/cvmmap-client/app_cvmmap_client.cpp:52-98` — shared memory assumptions
  - Pattern: `cv-mmap-gui/app/main.cpp:150-219` — frame buffer and shared mat conversion
  - Pattern: `cv-mmap-gui/app/aux-img/inc/app_aux_img.hpp:40-46` + `aux.cpp:54-99` — format/depth mapping

  **Acceptance Criteria** (agent-executable only):
  - [ ] GUI parser reads v1 and v2 metadata correctly.
  - [ ] Depth plane is accessible in application path (with documented behavior).
  - [ ] Left-plane render remains functional.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: v2 frame decode with depth availability
    Tool: Bash
    Steps: Run GUI-side parser/unit smoke against v2 fixture stream.
    Expected: slot0 left image parsed/rendered; slot1 depth available for downstream use.
    Evidence: .sisyphus/evidence/task-8-gui-v2-happy.txt

  Scenario: v1 backward compatibility
    Tool: Bash
    Steps: Run GUI against legacy v1 producer fixture/source.
    Expected: no crash; left rendering unchanged.
    Evidence: .sisyphus/evidence/task-8-gui-v1-compat-error.txt
  ```

  **Commit**: YES | Message: `feat(gui): support abi v2 and expose depth plane path` | Files: [`cv-mmap-gui/app/cvmmap-client/*`, `cv-mmap-gui/app/main.cpp`, `cv-mmap-gui/app/aux-img/*`]

- [x] 9. Harmonize control/sync version compatibility policy for migration window

  **What to do**: Decide and implement explicit compatibility policy: SHM metadata may be v2 while sync/control message major remains v1 during migration. Update validations and docs so mismatches are intentional and test-covered.
  **Must NOT do**: Do not introduce ambiguous implicit behavior where one component silently rejects migration traffic.

  **Recommended Agent Profile**:
  - Category: `deep` — Reason: protocol policy + runtime checks.
  - Skills: `[]`
  - Omitted: `frontend-ui-ux`.

  **Parallelization**: Can Parallel: YES | Wave 3 | Blocks: [10,11,12] | Blocked By: [1]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/src/main.cpp:551-555` — control request major-version check
  - Pattern: `cvmmap-python-client/src/cvmmap/msg.py:102-105` — sync version leniency
  - Pattern: `cv-mmap-gui/app/cvmmap-client/app_cvmmap_client.cpp:83-89` — major-version check in SHM verify

  **Acceptance Criteria** (agent-executable only):
  - [ ] Version policy documented and implemented consistently.
  - [ ] Migration window tests confirm no false rejection in intended combinations.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Intended mixed-version acceptance
    Tool: Bash
    Steps: Run integration test where SHM major=2 and control/sync major=1.
    Expected: consumer and control operations proceed per documented policy.
    Evidence: .sisyphus/evidence/task-9-version-policy-happy.txt

  Scenario: Explicit incompatible major rejection
    Tool: Bash
    Steps: Send control request with unsupported major and observe server handling.
    Expected: deterministic invalid-version response.
    Evidence: .sisyphus/evidence/task-9-version-policy-error.txt
  ```

  **Commit**: YES | Message: `chore(protocol): codify version compatibility during v2 rollout` | Files: [`cv-mmap/src/main.cpp`, `cvmmap-python-client/src/cvmmap/msg.py`, `cv-mmap-gui/app/cvmmap-client/*`, docs/tests]

- [x] 10. Build golden fixture suite and parser tests for v1/v2 across consumers

  **What to do**: Add reproducible binary fixtures for v1 and v2 metadata+payload, plus malformed variants. Wire tests in Python and GUI repositories to validate parser behavior and invariants.
  **Must NOT do**: Do not rely only on ad-hoc runtime manual checks.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` — Reason: cross-repo test assets and parser validation.
  - Skills: `[]`
  - Omitted: `playwright`.

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [12,13] | Blocked By: [4,5,7,8,9]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/docs/cvmmap.ksy:226-277` — normalized v1 guidance
  - Pattern: `cv-mmap/docs/cvmmap.ksy:375-419` — v2 invariants
  - Test: `cvmmap-python-client/tests/test_import_and_protocol.py` — current baseline tests
  - Pattern: `cv-mmap-gui/app/cvmmap-client/app_cvmmap_client.cpp:70-90` — current verify behavior

  **Acceptance Criteria** (agent-executable only):
  - [ ] Fixtures include valid v1, valid v2, and malformed v2 cases.
  - [ ] Python and GUI tests consume same fixture semantics.
  - [ ] Test runs are deterministic and CI-suitable.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Fixture-driven parser pass
    Tool: Bash
    Steps: Run `python -m pytest -q` in python client and GUI parser test command.
    Expected: valid fixtures pass in both repos.
    Evidence: .sisyphus/evidence/task-10-fixtures-happy.txt

  Scenario: Fixture-driven parser fail
    Tool: Bash
    Steps: Run tests against malformed fixture set.
    Expected: both repos reject malformed metadata with explicit failure reasons.
    Evidence: .sisyphus/evidence/task-10-fixtures-error.txt
  ```

  **Commit**: YES | Message: `test(protocol): add shared v1/v2 fixtures and invariant checks` | Files: [`cvmmap-python-client/tests/*`, `cv-mmap-gui/*tests*`, fixture assets]

- [x] 11. Execute cross-repo runtime interoperability matrix

  **What to do**: Run end-to-end matrix covering producer backends and consumer combinations, including ZED OFF/ON builds and network/local stream modes where available. Capture explicit pass/fail evidence for each matrix cell.
  **Must NOT do**: Do not declare completion without matrix evidence artifacts.

  **Recommended Agent Profile**:
  - Category: `deep` — Reason: multi-repo integration and sequencing validation.
  - Skills: `[]`
  - Omitted: `frontend-ui-ux`.

  **Parallelization**: Can Parallel: YES | Wave 4 | Blocks: [12,13] | Blocked By: [4,5,7,8,9]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/src/main.cpp:347-396` — backend selection and ZED build gating
  - Pattern: `cv-mmap/app/config/app_config.cpp:347-356` — backend section requirements
  - Pattern: `cvmmap-python-client/src/cvmmap/__init__.py:200-257` — async consumption flow
  - Pattern: `cv-mmap-gui/app/main.cpp:289-305` — GUI client callback/render pipeline entry

  **Acceptance Criteria** (agent-executable only):
  - [ ] Matrix includes: v1 fixture, v2 left+depth, malformed v2, ZED OFF build, ZED ON build.
  - [ ] Python and GUI both verified against v2 producer path.
  - [ ] No regression in control reset command behavior.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Interop matrix pass cells
    Tool: Bash
    Steps: Execute scripted matrix runner across repos/builds/configs.
    Expected: all required pass cells succeed and are logged.
    Evidence: .sisyphus/evidence/task-11-interop-matrix-happy.txt

  Scenario: Interop matrix fail cell
    Tool: Bash
    Steps: Run matrix cell with intentionally mismatched unsupported major.
    Expected: deterministic fail with explicit compatibility error.
    Evidence: .sisyphus/evidence/task-11-interop-matrix-error.txt
  ```

  **Commit**: YES | Message: `test(integration): add cross-repo abi v2 interoperability matrix` | Files: [matrix scripts/tests/evidence index]

- [x] 12. Final hardening, documentation sync, and release sequencing notes

  **What to do**: Consolidate protocol docs, migration notes, and rollout order (consumer readiness before producer-only-v2 deployments). Ensure config examples and usage docs reflect strict validation and active ethernet behavior.
  **Must NOT do**: Do not leave conflicting docs between plan/spec/code.

  **Recommended Agent Profile**:
  - Category: `writing` — Reason: docs precision and release notes.
  - Skills: `[]`
  - Omitted: `playwright`.

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [13] | Blocked By: [3,4,5,6,7,8,9,10,11]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/docs/cvmmap.ksy` — canonical protocol doc
  - Pattern: `cv-mmap/config_example.toml:14-30` — zed config examples
  - Pattern: `cvmmap-python-client/README.md` — client expectations
  - Pattern: `cv-mmap-gui/README.md` — GUI usage context

  **Acceptance Criteria** (agent-executable only):
  - [ ] Docs and examples reflect implemented validation rules and ABI behavior.
  - [ ] Rollout sequencing note explicitly included.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Doc consistency check
    Tool: Bash
    Steps: Search docs/config for obsolete fallback semantics and v1-only assumptions.
    Expected: no stale contradictions found.
    Evidence: .sisyphus/evidence/task-12-doc-sync-happy.txt

  Scenario: Stale reference detection
    Tool: Bash
    Steps: Run grep checks for banned legacy phrases (e.g., warn+fallback on unknown zed modes).
    Expected: zero matches.
    Evidence: .sisyphus/evidence/task-12-doc-sync-error.txt
  ```

  **Commit**: YES | Message: `docs(release): sync abi v2 rollout and zed ethernet behavior` | Files: [`cv-mmap/docs/*`, `cv-mmap/config_example.toml`, `cvmmap-python-client/README.md`, `cv-mmap-gui/README.md`]

- [x] 13. Final repository-level verification sweep and sign-off package

  **What to do**: Execute final build/test/package checklist per repo and produce a single sign-off report containing command outputs, matrix status, known limitations, and rollback notes.
  **Must NOT do**: Do not merge/release without complete evidence package.

  **Recommended Agent Profile**:
  - Category: `unspecified-high` — Reason: integrated quality gate.
  - Skills: `[]`
  - Omitted: `frontend-ui-ux`.

  **Parallelization**: Can Parallel: NO | Wave 4 | Blocks: [] | Blocked By: [10,11,12]

  **References** (executor has NO interview context — be exhaustive):
  - Pattern: `cv-mmap/AGENTS.md` — project constraints and commands
  - Pattern: This plan’s Definition of Done and matrix tasks
  - Test: all generated evidence files `.sisyphus/evidence/task-*`

  **Acceptance Criteria** (agent-executable only):
  - [ ] All required commands pass and are archived in evidence.
  - [ ] Sign-off report includes pass/fail table and known limitations.

  **QA Scenarios** (MANDATORY — task incomplete without these):
  ```
  Scenario: Full verification pass
    Tool: Bash
    Steps: Run final verification script for all repos.
    Expected: all gates pass; report generated.
    Evidence: .sisyphus/evidence/task-13-final-verification-happy.txt

  Scenario: Gate failure handling
    Tool: Bash
    Steps: Simulate one failed gate in dry-run mode.
    Expected: report marks release blocked with failing gate details.
    Evidence: .sisyphus/evidence/task-13-final-verification-error.txt
  ```

  **Commit**: YES | Message: `chore(release): add final verification sign-off package` | Files: [verification scripts/reports]

## Final Verification Wave (4 parallel agents, ALL must APPROVE)
- [x] F1. Plan Compliance Audit — oracle
- [x] F2. Code Quality Review — unspecified-high
- [x] F3. Real Manual QA — unspecified-high (+ playwright if UI)
- [x] F4. Scope Fidelity Check — deep

## Commit Strategy
- Keep commits atomic by repo boundary:
  - Commit set A: `cv-mmap` ABI/config/ZED changes
  - Commit set B: `cvmmap-python-client` parser/API/tests
  - Commit set C: `cv-mmap-gui` parser/render/tests
- Conventional commit format required.

## Success Criteria
- Producer emits valid ABI v2 metadata with deterministic plane descriptors.
- Python and GUI parse both v1 and v2; depth plane exposed without regressing plane-0 path.
- ZED network stream mode remains operational with stricter validation.
- Build/test matrix passes with evidence files for each task.
