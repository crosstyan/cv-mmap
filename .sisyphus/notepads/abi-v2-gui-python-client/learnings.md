

## 2026-03-04: ABI Compatibility Test Patterns Research

### Patterns Found

#### 1. Golden-Binary Fixtures
- Source: TensorFlow Lite testdata/*.bin files
- Pattern: Store canonical binary blobs in testdata/ directories  
- Use: Version-controlled binary files for protocol versions
- Benefit: Catches layout changes immediately

#### 2. Cross-Language Struct Layout Validation  
- Source: Godot Engine extension_api_dump.cpp
- Pattern: static_assert sizeof and offsetof checks
- Use: Compile-time verification of struct sizes
- Benefit: Fails at compile time

#### 3. Hypothesis-Based Property Testing
- Source: Python CPython test_base64.py
- Pattern: hypothesis.given with binary strategies
- Use: Generate random inputs for parser testing
- Benefit: Finds edge cases developers miss

#### 4. libFuzzer Integration
- Source: Google Skia fuzz/, LLVM project
- Pattern: LLVMFuzzerTestOneInput function
- Use: Coverage-guided fuzzing of parsers
- Benefit: Finds crashes and memory corruption

#### 5. Kaitai Struct for Format Validation
- Source: cv-mmap docs/cvmmap.ksy
- Pattern: Schema-first binary format definition
- Use: Generate parsers from single source of truth
- Benefit: Ensures C++ and Python agreement

### Key Tools for ABI Testing

| Tool | Purpose |
|------|---------|
| static_assert sizeof | Compile-time size check |
| static_assert offsetof | Compile-time offset check |
| Golden fixtures | Canonical binary reference |
| Hypothesis | Property-based testing |
| libFuzzer | Coverage-guided fuzzing |
| kaitai-struct | Schema validation |

### Desync Detection Matrix

- Struct size change: static_assert sizeof
- Field offset change: static_assert offsetof  
- Alignment mismatch: Round-trip test
- Endianness bug: Golden fixture
- Truncated data: Malformed test
- Invalid magic: Parser validation
- [2026-03-04 14:59:03Z] Refreshed  as the single ABI authority: corrected control magic values to 0x3C/0x3D, aligned v1  field widths/enums with C++ (), and replaced stale SHM metadata layout with explicit version-dispatched  (major=1/2).
- [2026-03-04 14:59:03Z] Added explicit v2 contract sections (, , ) with fixed offsets/sizes, packed layout assumptions, and deterministic plane ordering + bounds invariants so GUI/Python parsers can share identical checks.
- [2026-03-04 14:59:03Z] Documented normative v1 normalization into a synthetic single plane (, offset=0, size=buffer_size, derived stride when divisible) plus  guard semantics to prevent unsafe slicing in consumers.
- [2026-03-04 14:59:21Z] Refreshed docs/cvmmap.ksy as the single ABI authority: corrected control magic values to 0x3C/0x3D, aligned v1 frame_info field widths/enums with C++ (u2/u2/u1/u1/u1/u1/u4), and replaced stale SHM metadata layout with explicit version-dispatched frame_metadata (major=1/2).\n- [2026-03-04 14:59:21Z] Added explicit v2 contract sections (frame_metadata_v2_header, frame_plane_descriptor_v2, frame_metadata_v2) with fixed offsets/sizes, packed layout assumptions, and deterministic plane ordering + bounds invariants so GUI/Python parsers can share identical checks.\n- [2026-03-04 14:59:21Z] Documented normative v1 normalization into a synthetic single plane (plane_count=1, offset=0, size=buffer_size, derived stride when divisible) plus normalization_valid guard semantics to prevent unsafe slicing in consumers.\n- [2026-03-04 15:08:24Z] Recovery run constrained working tree to Task-1 scope only: reverted all non-ABI files and removed stray artifacts before continuing schema work.
- [2026-03-04 15:08:24Z] v1 binary mapping in docs/cvmmap.ksy now matches C++ layout exactly: magic[8], version bytes, reserved_0[2], frame_count(u4), timestamp_ns(u8), frame_info(12), explicit 4-byte tail alignment, then reserved bytes to SHM payload offset 256.
- [2026-03-04 15:08:24Z] v2 contract explicitly documents fixed header/descriptor sizes (64/24), deterministic plane slot ordering, contiguous payload offsets, and bounds checks to prevent out-of-range slicing in downstream consumers.
