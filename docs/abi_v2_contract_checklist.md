# ABI v2 Contract Checklist

**Source of Truth:** `docs/cvmmap.ksy`  
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

## Implementation Target Mapping: cv-mmap

### Current v1 Implementation (Legacy)

| KSY v2 Field | Current C++ Location | Status | Notes |
|--------------|---------------------|--------|-------|
| `magic[8]` | `app/models/app_metadata_models.hpp:199` | EXISTS | `uint8_t magic[CV_MMAP_MAGIC.size()]` |
| `versions_major` | `app/models/app_metadata_models.hpp:200` | EXISTS v1 | `uint8_t versions_major{VERSION_MAJOR}` |
| `versions_minor` | `app/models/app_metadata_models.hpp:201` | EXISTS v1 | `uint8_t versions_minor{VERSION_MINOR}` |
| `flags` | - | **TODO** | Add u16 flags field |
| `frame_id` | - | **TODO** | Replace `frame_count` with `frame_id` |
| `capture_ts_ns` | `app/models/app_metadata_models.hpp:204` | EXISTS | `uint64_t timestamp_ns` |
| `publish_seq` | - | **TODO** | Add u64 publish sequence |
| `plane_count` | - | **TODO** | Add u8 plane count (1..4) |
| `plane_presence_mask` | - | **TODO** | Add u8 presence mask |
| `plane_descriptors_offset` | - | **TODO** | Add u16 descriptor offset (fixed 64) |
| `plane_descriptor_size` | - | **TODO** | Add u16 descriptor size (fixed 24) |
| `plane_descriptor_capacity` | - | **TODO** | Add u16 descriptor capacity (fixed 4) |
| `payload_size_bytes` | - | **TODO** | Add u32 payload size |
| `reserved_0[20]` | - | **TODO** | Add 20 reserved bytes |

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
| `VERSION_MAJOR` | `1` | `app/models/app_common_models.hpp:35` | `2` |
| `VERSION_MINOR` | `0` | `app/models/app_common_models.hpp:36` | `0` |

---

## Migration Checklist: cv-mmap C++ Implementation

### Phase 1: Header Structure Update

- [ ] Create `frame_metadata_v2_header_t` struct (64 bytes)
- [ ] Create `frame_plane_descriptor_v2_t` struct (24 bytes)
- [ ] Create `frame_metadata_v2_t` struct (256 bytes total)
- [ ] Update `VERSION_MAJOR` to `2` in `app_common_models.hpp`

### Phase 2: Validation Logic

- [ ] Implement `plane_presence_mask` validation: `(_ & 0xF0) == 0`
- [ ] Implement `plane_count` validation: `_ >= 1 && _ <= 4`
- [ ] Implement `contiguous_mask_valid` check
- [ ] Implement `is_empty_descriptor` check for inactive slots
- [ ] Implement plane bounds validation (`plane_N_in_bounds`)
- [ ] Implement plane slot 0 validation (`plane_0_type_valid`, etc.)

### Phase 3: Serialization

- [ ] Implement `marshal()` for v2 header
- [ ] Implement `unmarshal()` for v2 header
- [ ] Ensure no implicit padding (use packed attribute or static_asserts)

### Phase 4: Tests

- [ ] Add static_assert for header size: `sizeof(frame_metadata_v2_header_t) == 64`
- [ ] Add static_assert for descriptor size: `sizeof(frame_plane_descriptor_v2_t) == 24`
- [ ] Add static_assert for full metadata: `sizeof(frame_metadata_v2_t) == 256`
- [ ] Add tests for presence mask validation
- [ ] Add tests for plane ordering invariants

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
grep -n "plane_descriptor_size\|plane_descriptor_capacity\|plane_presence_mask" docs/cvmmap.ksy
```

Expected output should include:
- Line 352: `plane_presence_mask`
- Line 358-360: `plane_descriptor_size`
- Line 361-363: `plane_descriptor_capacity`
