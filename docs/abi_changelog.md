# ABI Changelog

This document records wire-compatible ABI changes that affect downstream
parsers, transports, and generated schemas.

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
