# Project Assessment And Refactor Notes

Date: 2026-04-24

This document records the maintained version of the project-structure review.
Historical `.sisyphus` task plans and evidence logs have been retired; durable
protocol details now belong in the ABI docs, and durable engineering notes live
here.

## Summary Judgment

`cv-mmap` is a reasonable C++ project for its current problem space. The split
between installable consumer-facing protocol/client code and producer-only
runtime code is sound:

- `core/` owns the public C++ package, ABI structs, protocol parsing, target
  resolution, NATS helpers, and consumer client APIs.
- `app/` owns producer configuration, backend implementations, metadata models,
  preprocessing, and version/build metadata.
- `src/` owns the executable-level runtime glue: CLI, backend orchestration,
  shared-memory publication, and process lifecycle.

The project is not small anymore. A few files carry most of the complexity:

- `app/config/app_config.cpp`: TOML loading, inheritance, parsing,
  normalization, validation, and serialization.
- `src/frame_publisher.cpp`: SHM lifecycle, ABI v2/v2.1 metadata construction,
  sync publication, encoded-AU pairing, and direct/copy frame paths.
- `app/backends/zed/zed_capture_pipeline.cpp`: ZED capture cadence, depth and
  confidence retrieval, fallback policy, direct SHM output bindings, SVO/live
  behavior, and source state.
- `core/src/nats_client.cpp` and `core/src/nats_service.cpp`: transport,
  discovery, control, status, and service wiring.

That concentration is understandable given the domain: this is not just a video
reader, it is a producer/runtime with a stable shared-memory ABI, optional
hardware SDKs, finite-source control, NATS discovery/control, and downstream
cross-language consumers.

## Current Complexity

Complexity level: medium-high.

Primary drivers:

- ABI stability: struct sizes, offsets, parser behavior, and generated fixture
  contracts must stay aligned across C++, Python, GUI, and streamer consumers.
- Multi-backend behavior: dummy, OpenCV, GStreamer, UDP/RTP, MCAP, and ZED have
  different source semantics but converge into one frame publication contract.
- Optional dependencies: OpenCV, GStreamer, MCAP, RVL, ZED, CUDA, and NATS need
  clear build-time behavior.
- Runtime modes: live cameras, SVO playback, playlists, finite looping, direct
  SHM frame fills, copy-based frame publishing, depth cadence, body tracking,
  encoded access units, and recording controls all share one executable.
- Cross-transport contract: frame data uses SHM+ZMQ sync, while control/status
  and body tracking use NATS when enabled.

This complexity is real, but mostly domain-driven. The project becomes risky
when multiple concerns are implemented in the same long translation unit rather
than behind small, testable policy objects.

## Good Structure

The current shape has several strong points:

- `cvmmap-core` is installable and exports clear CMake targets.
- Public protocol definitions live under `core/include/cvmmap/`.
- Parser tests cover v1, v2, v2.1, depth units, confidence, encoded access
  units, and malformed metadata.
- The dummy backend gives deterministic producer behavior for local and
  downstream tests.
- ABI documentation exists in `docs/` and is close enough to the code that drift
  can be found by inspection.
- Optional backend modes use explicit CMake choices instead of silently enabling
  everything.
- The ZED backend has already been split into lifecycle, capture, controls,
  recording, and SDK utility files.

## Refactor Chances

These are ordered by expected payoff.

1. [x] Split config loading by responsibility.

   `app/config/app_config.cpp` should become smaller modules or at least smaller
   internal sections:

   - TOML inheritance and table merge helpers
   - scalar/list parsing helpers
   - per-backend config parsers
   - per-backend validators
   - serialization back to TOML

   The goal is not abstraction for its own sake. The current file is large
   enough that unrelated config changes are likely to conflict and review poorly.

   Completed in `b6c54eb refactor(config): split app config implementation`.

2. [x] Make frame layout explicit before metadata construction.

   `FramePublisher::BuildV2Metadata` currently infers LEFT/DEPTH/CONFIDENCE
   planes from packed payload sizes. That keeps the backend callback ABI stable,
   but it is now brittle because the producer also supports v2.1 encoded access
   units.

   A better boundary would be a small `FrameLayout` or `PlaneSet` value with:

   - explicit slot descriptors
   - total payload size
   - depth unit
   - encoded extension metadata

   Backends could still fill one packed payload, but the publisher would stop
   guessing what that payload means.

   Completed in `7c2cadb refactor(frame): publish explicit payload layouts`.

3. [x] Extract protocol builder/validator helpers.

   Producer metadata creation and consumer parser validation currently mirror
   each other by convention. A small shared protocol utility layer could own:

   - v2/v2.1 mask validation
   - descriptor bounds checks
   - slot type rules
   - encoded extension checks
   - C++ fixture construction helpers

   Keep parser error messages in `core`, but avoid duplicating the same layout
   arithmetic in tests and producer code.

   Completed in this wave by adding shared ABI v2/v2.1 protocol helpers under
   `cvmmap_ipc` and wiring parser, parser fixtures, and producer metadata
   construction through them.

4. [ ] Continue shrinking ZED capture policy.

   `zed_capture_pipeline.cpp` is already separated from lifecycle, but it still
   contains several policies:

   - depth cadence
   - fallback depth cache
   - confidence availability
   - direct output bindings
   - compact copy path
   - source info and finite-source state

   Splitting the depth/confidence packing and fallback policy into a small
   testable component would reduce risk without changing the backend facade.

5. [ ] Split NATS client/service by API area.

   `core/src/nats_client.cpp` and `core/src/nats_service.cpp` are large because
   discovery, control, status, body transport, and request serialization are
   mixed together. Good split points are:

   - discovery
   - control RPC
   - body/status subscriptions
   - protobuf encode/decode helpers
   - reconnect/error policy

6. [ ] Normalize CMake backend dependency helpers.

   `app/CMakeLists.txt` has useful backend-mode logic, but dependency discovery
   is still embedded in one file. Backend-specific helper functions would make
   optional dependency failures easier to read and less prone to copy-paste
   differences.

## Documentation Alignment

The current documentation should treat these as primary contracts:

- `docs/cvmmap_shm_metadata_v1_v2.ksy`
- `docs/cvmmap_sync_v1.ksy`
- `docs/cvmmap_body_tracking_v1.ksy`
- `core/proto/cvmmap/control.proto`
- `core/include/cvmmap/ipc.hpp`
- `docs/abi_changelog.md`
- `docs/abi_v2_contract_checklist.md`
- `docs/abi_v2_migration_guide.md`

The biggest alignment issue found in this pass was ABI v2.1 drift: code and
tests already support sparse masks and slot-3 encoded access units, while the
KSY/checklist still described only the base contiguous v2 model. The maintained
docs now need to describe v2.0 and v2.1 separately.

Avoid adding absolute local checkout paths to user-facing docs. Refer to
downstream projects by repository name and only mention sibling checkout layout
as a convention.

## Retired `.sisyphus` Material

Useful material from `.sisyphus` was consolidated as follows:

- ABI v2/v2.1 rules belong in `docs/abi_v2_contract_checklist.md`,
  `docs/abi_v2_migration_guide.md`, and `docs/abi_changelog.md`.
- ZED depth and confidence behavior belongs in backend docs and in the parser
  contract where it affects shared-memory layout.
- Cross-repo consumer alignment belongs in `docs/python-client.md`, the
  installed fixtures under `core/fixtures/`, and downstream test suites.
- Historical runner scripts, transient evidence files, agent task plans, and
  append-only notepads are not maintained project documentation.

The last historical `.sisyphus` interop note recorded an 11/11 matrix pass on
2026-03-05. Treat that as historical context only; current validation should be
done by building this repo and running the checked-in tests/fixtures.

## Review Guidance

For future reviews, focus first on contract drift and long-file risk:

- Does an ABI change update the KSY, C++ structs, parser, fixtures, changelog,
  and downstream compatibility docs?
- Does a backend change preserve the frame publication contract for LEFT,
  DEPTH, CONFIDENCE, and ENCODED_ACCESS_UNIT slots?
- Does a config change validate at parse time and serialize back clearly?
- Does a new optional dependency fail clearly when requested and stay disabled
  when not requested?
- Is a large file getting larger because it owns another policy that could be
  tested separately?
