# ABI Changelog

This document records wire-compatible ABI changes that affect downstream
parsers, transports, and generated schemas.

## SHM Metadata v2 Depth Unit

### 2026-03-11

Status:
- SHM metadata wire version unchanged (`versions_major = 2`, `versions_minor = 0`)
- fixed v2 header size unchanged (`64` bytes)
- plane descriptor layout unchanged

Change:
- the v2 SHM header byte at offset `0x2C` is now part of the public ABI contract
  as `depth_unit` (`DepthUnit`)
  - `0 = unknown`
  - `1 = millimeter`
  - `2 = meter`
- the remaining bytes `0x2D..0x3F` stay reserved

Reason:
- downstream consumers that record or transform depth need an explicit metric-unit
  contract for the existing `GRAY/F32` depth plane
- ZED producers can emit depth in either millimeters or meters depending on
  coordinate-unit configuration, and that must not remain implicit

Downstream impact:
- `core` parser validates and exposes `depth_unit`
- `frame_planes_view_t` carries `depth_unit` alongside optional depth/confidence planes
- `cvmmap_shm_metadata_v1_v2.ksy` parses the byte explicitly

Compatibility:
- old v2 packets with zeroed reserved bytes still parse as `depth_unit = unknown`
- this is a semantic extension of v2, not a version bump

## Body Tracking PUB Header

### 2026-03-07

Status:
- wire version unchanged (`VERSION_MAJOR = 1`, `VERSION_MINOR = 0`)
- fixed header size unchanged (`64` bytes)
- no body-record layout change

Change:
- the body-tracking header bytes at offsets `0x01` and `0x22` are now part of the
  public ABI contract:
  - `coordinate_system_code` (`BodyCoordinateSystem`)
  - `reference_frame_code` (`BodyReferenceFrame`)
- `flags & 0x0010` is now defined as `floor_as_origin`

Reason:
- native ZED body packets need to tell downstreams which coordinate system and
  reference frame the 3D joints use
- `set_floor_as_origin` materially changes world-frame semantics and must not be
  implicit producer-side state

Downstream impact:
- `core` parser validates and exposes these fields
- `cvmmap_body_tracking_v1.ksy` parses them explicitly
- `cvmmap-python-client` exposes them on `BodyTrackingMessageHeader` and
  `BodyFrame`

Compatibility:
- old packets with zero/default values still parse as
  `coordinate_system = unknown`,
  `reference_frame = unknown`,
  `floor_as_origin = false`
- this is a semantic extension of v1, not a version bump
