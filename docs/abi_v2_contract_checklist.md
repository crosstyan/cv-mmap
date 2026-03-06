# ABI v2 Contract Checklist

**Source of Truth:** `docs/cvmmap_shm_metadata_v1_v2.ksy`  
**Contract Version:** v2 (major=2)  
**Generated:** 2026-03-04

This document maps ALL v2 header/descriptor invariants to exact target code locations.

---

## Normative References

| Document | Line Range | Description |
|----------|------------|-------------|
| `docs/cvmmap.ksy` | 314-369 | `frame_metadata_v2_header` struct definition |
| `docs/cvmmap.ksy` | 375-433 | `frame_metadata_v2` struct definition (full metadata) |
| `docs/cvmmap.ksy` | 276-312 | `frame_plane_descriptor_v2` struct definition |
| `docs/cvmmap.ksy` | 384-390 | Deterministic plane ordering rules |

---

## v2 Header Invariants (64 bytes, packed)

### Byte Layout (Normative)

| Offset | Field | Type | KSY Line | Valid/Constraint |
|--------|-------|------|----------|------------------|
| 0x00 | `magic[8]` | bytes | 334-335 | Must be `[67, 86, 45, 77, 77, 65, 80, 0]` ("CV-MMAP\0") |
| 0x08 | `versions_major` | u1 | 336-338 | Must equal `2` |
| 0x09 | `versions_minor` | u1 | 339-340 | Any u8 value |
| 0x0A | `flags` | u2 | 341-342 | Currently reserved (u16) |
| 0x0C | `frame_id` | u4 | 343-344 | Any u32 value |
| 0x10 | `capture_ts_ns` | u8 | 345-346 | Any u64 value |
| 0x18 | `publish_seq` | u8 | 347-348 | Any u64 value |
| 0x20 | `plane_count` | u1 | 349-351 | Must be in range `[1, 4]` |
| 0x21 | `plane_presence_mask` | u1 | 352-354 | Upper 4 bits must be 0: `(_ & 0xF0) == 0` |
| 0x22 | `plane_descriptors_offset` | u2 | 355-357 | Must equal `64` |
| 0x24 | `plane_descriptor_size` | u2 | 358-360 | Must equal `24` |
| 0x26 | `plane_descriptor_capacity` | u2 | 361-363 | Must equal `4` |
| 0x28 | `payload_size_bytes` | u4 | 364-366 | Must be greater than `0` |
| 0x2C | `reserved_0[20]` | bytes | 367-368 | 20 reserved bytes |

### KSY Instance Constraints

| Instance | Expression | KSY Line | Meaning |
|----------|------------|----------|---------|
| `contiguous_mask_expected` | `(1 << plane_count) - 1` | 370-371 | Expected bit pattern for contiguous plane mask |
| `contiguous_mask_valid` | `plane_presence_mask == contiguous_mask_expected` | 372-373 | Presence mask must match expected contiguous pattern |

---

## v2 Plane Descriptor Invariants (24 bytes, packed)

### Byte Layout (Normative)

| Offset | Field | Type | KSY Line | Description |
|--------|-------|------|----------|-------------|
| 0x00 | `plane_type` | u1 | 288-289 | Enum: `frame_plane_type` |
| 0x01 | `pixel_format` | u1 | 291-293 | Enum: `pixel_format` |
| 0x02 | `depth` | u1 | 294-296 | Enum: `depth` |
| 0x03 | `reserved_0` | u1 | 297-299 | Explicit reserved byte |
| 0x04 | `width` | u4 | 300-301 | Width in pixels |
| 0x08 | `height` | u4 | 302-303 | Height in pixels |
| 0x0C | `stride_bytes` | u4 | 304-305 | Stride in bytes |
| 0x10 | `offset_bytes` | u4 | 306-307 | Offset into payload |
| 0x14 | `size_bytes` | u4 | 308-309 | Size of plane data in bytes |

### Descriptor Invariants (KSY Lines 280-287)

1. `offset_bytes <= payload_size_bytes`
2. `size_bytes <= payload_size_bytes - offset_bytes`
3. Active descriptors must have non-zero width/height/stride/size
4. Inactive descriptors must be all-zero for geometric + byte-range fields

### KSY Instance Helpers

| Instance | Expression | KSY Line | Meaning |
|----------|------------|----------|---------|
| `is_empty_descriptor` | `width == 0 and height == 0 and stride_bytes == 0 and offset_bytes == 0 and size_bytes == 0` | 311-312 | True if descriptor is inactive/empty |

---

## v2 Full Metadata Layout (256 bytes)

| Region | Offset Range | Content | KSY Lines |
|--------|--------------|---------|-----------|
| Header | [0..63] | `frame_metadata_v2_header` | 392-393 |
| Descriptors | [64..159] | 4 x `frame_plane_descriptor_v2` (fixed slots) | 394-401 |
| Reserved | [160..255] | Reserved/padding | 402-404 |
| **Payload** | **256+** | **Frame data begins here** | - |

---

## Deterministic Plane Ordering Rules (KSY Lines 384-390)

**Rule 1:** Active descriptors are contiguous from slot 0.  
**Rule 2:** Slot 0 is always LEFT plane.  
**Rule 3:** Slot 1 is DEPTH plane when `plane_count >= 2`.  
**Rule 4:** Slots >= plane_count are inactive and must be empty descriptors.  
**Rule 5:** Active planes are packed in payload order: each next offset equals the previous offset + previous size.

### Plane Slot Semantics

| Slot | Plane Type | Condition |
|------|------------|-----------|
| 0 | `left` | Always active (plane_count >= 1) |
| 1 | `depth` | Active when plane_count >= 2 |
| 2 | Reserved | Active when plane_count >= 3 |
| 3 | Reserved | Active when plane_count == 4 |

---

## Implementation Target Mapping: cv-mmap / cvmmap-core

### Current v2 Implementation

| KSY v2 Field | Producer-side C++ Location | Consumer-side C++ Location | Status |
|--------------|----------------------------|----------------------------|--------|
| `magic[8]` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `versions_major` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `versions_minor` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `flags` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `frame_id` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `capture_ts_ns` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `publish_seq` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `plane_count` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `plane_presence_mask` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `plane_descriptors_offset` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `plane_descriptor_size` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `plane_descriptor_capacity` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `payload_size_bytes` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |
| `reserved_0[20]` | `app/models/app_metadata_models.hpp` | `core/include/cvmmap/ipc.hpp` | Implemented |

### Current v1 `frame_info_t` (12 bytes)

| Field | C++ Location | Size | Notes |
|-------|--------------|------|-------|
| `width` | `app/models/app_metadata_models.hpp:134` | u16 | Same as v2 |
| `height` | `app/models/app_metadata_models.hpp:135` | u16 | Same as v2 |
| `channels` | `app/models/app_metadata_models.hpp:136` | u8 | Replaced by `pixel_format` in v2 |
| `depth` | `app/models/app_metadata_models.hpp:138` | u8 | Same as v2 |
| `pixel_format` | `app/models/app_metadata_models.hpp:139` | u8 | Same as v2 |
| `_reserved_0[1]` | `app/models/app_metadata_models.hpp:140` | u8 | Padding |
| `buffer_size` | `app/models/app_metadata_models.hpp:141` | u32 | Replaced by per-plane `size_bytes` in v2 |

### Version Constants

| Constant | Current Value | Location | Target v2 |
|----------|---------------|----------|-----------|
| `FRAME_METADATA_V2_MAJOR` | `2` | `core/include/cvmmap/ipc.hpp` | `2` |
| `VERSION_MINOR` | `0` | `app/models/app_common_models.hpp:36` | `0` |

---

## Current Verification Checklist: cv-mmap C++ Implementation

### Header Structure

- [x] `frame_metadata_v2_header_t` exists (64 bytes)
- [x] `frame_plane_descriptor_v2_t` exists (24 bytes)
- [x] `frame_metadata_v2_t` exists (256 bytes total)
- [x] consumer-side public definitions are exported through `core/include/cvmmap/ipc.hpp`

### Validation Logic

- [x] `plane_presence_mask` validation implemented in parser
- [x] `plane_count` validation implemented in parser
- [x] contiguous mask validation implemented in parser
- [x] empty-descriptor validation implemented in parser
- [x] plane bounds validation implemented in parser
- [x] plane slot ordering validation implemented in parser

### Serialization / Layout

- [x] producer-side v2 layout is defined with explicit offsets and static assertions
- [x] consumer-side parser accepts v1 and v2 SHM metadata
- [x] no implicit layout drift is allowed for the exported v2 structs

### Tests / Contract Sources

- [x] static assertions exist for 64-byte header
- [x] static assertions exist for 24-byte descriptor
- [x] static assertions exist for 256-byte metadata region
- [x] parser logic validates presence masks and plane ordering invariants
- [x] `docs/cvmmap_shm_metadata_v1_v2.ksy` is the versioned SHM format document

---

## Reference: KSY Type Enums

### `frame_plane_type` (u1)

| Value | Name |
|-------|------|
| 0 | `left` |
| 1 | `depth` |

### `pixel_format` (u1)

| Value | Name |
|-------|------|
| 0 | `rgb` |
| 1 | `bgr` |
| 2 | `rgba` |
| 3 | `bgra` |
| 4 | `gray` |
| 5 | `yuv` |
| 6 | `yuyv` |

### `depth` (u1)

| Value | Name |
|-------|------|
| 0 | `u8` |
| 1 | `s8` |
| 2 | `u16` |
| 3 | `s16` |
| 4 | `s32` |
| 5 | `f32` |
| 6 | `f64` |
| 7 | `f16` |

---

## Verification

Run this grep to verify all normative fields are present in ksy:

```bash
grep -n "plane_descriptor_size\|plane_descriptor_capacity\|plane_presence_mask" docs/cvmmap_shm_metadata_v1_v2.ksy
```

Expected output should include:
- Line 352: `plane_presence_mask`
- Line 358-360: `plane_descriptor_size`
- Line 361-363: `plane_descriptor_capacity`
