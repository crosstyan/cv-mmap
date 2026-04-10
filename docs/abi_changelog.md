# ABI Changelog

This document records wire-compatible ABI changes that affect downstream
parsers, transports, and generated schemas.

## Control Wire Recording Commands

### 2026-03-15

Status:
- control wire major/minor unchanged (`VERSION_MAJOR = 1`, `VERSION_MINOR = 0`)
- request/response envelope unchanged
- on-wire header sizes remain:
  - request header = `34` bytes
  - response header = `38` bytes

Change:
- added control commands:
  - `START_RECORDING` (`0x1004`)
  - `STOP_RECORDING` (`0x1005`)
  - `GET_RECORDING_STATUS` (`0x1006`)
- added `SOURCE_INFO_FLAG_CAN_RECORD` (`0x00000010`)
- added payload structs:
  - `recording_start_request_v1`
  - `recording_status_response_v1`
- added `RecordingFormat` enum with `Svo`

Reason:
- ZED SVO recording must be controlled at the producer/backend layer because
  the ZED SDK camera object is the only place that can emit the proprietary
  SVO format
- downstreams still need a stable transport-independent control contract

Downstream impact:
- `cvmmap-core` clients can start, stop, and query recording state
- capability discovery can distinguish record-capable producers via
  `SOURCE_INFO_FLAG_CAN_RECORD`
- Kaitai parsers can validate the new request/response payloads

Compatibility:
- this is a compatible extension of control v1, not a version bump
- backends that do not implement recording return `UNSUPPORTED`

## Control Wire Source Info And Timestamp Seek

### 2026-03-13

Status:
- control wire major/minor unchanged (`VERSION_MAJOR = 1`, `VERSION_MINOR = 0`)
- request/response command envelope unchanged semantically
- on-wire header sizes explicitly documented as:
  - request header = `34` bytes
  - response header = `38` bytes
- C++ envelope structs remain `36` and `40` bytes because of unsent tail padding

Change:
- added control commands:
  - `GET_SOURCE_INFO` (`0x1002`)
  - `SEEK_TIMESTAMP_NS` (`0x1003`)
- added response codes:
  - `UNSUPPORTED` (`-7`)
  - `INVALID_PAYLOAD` (`-8`)
  - `OUT_OF_RANGE` (`-9`)
- added successful payload structs:
  - `source_info_response_v1`
  - `seek_timestamp_request_v1`
  - `seek_timestamp_response_v1`
- the legacy control v1 schema documented the real 34-byte / 38-byte wire
  envelope sizes for those packets before the control plane moved fully to
  protobuf over NATS

Reason:
- finite replay backends need a standard control-plane way to report source
  capabilities and position
- timestamp seek needs a stable wire contract independent of backend type
- downstream parsers must not infer wrong payload offsets from `sizeof(...)`
  on flexible-array C structs

Downstream impact:
- `core` client can query source kind/timestamp domain/flags and seek by timestamp
- replay-oriented consumers can distinguish finite vs live sources without
  backend-specific heuristics
- downstream parsers could validate control packets against the actual on-wire
  envelope size instead of host ABI padding

Compatibility:
- this is a compatible extension of control v1, not a version bump
- old consumers that only send `RESET_FRAME_COUNT` continue to work
- new consumers must treat non-`OK` response codes as having either no payload or
  command-specific future payloads

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
