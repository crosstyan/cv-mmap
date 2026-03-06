meta:
  id: cvmmap
  title: cv-mmap IPC Binary Formats
  endian: le
  license: MIT
doc: |
  Aggregate legacy Kaitai reference for cv-mmap IPC payloads.

  Prefer the split, version-tagged schemas for new work:
  - `docs/cvmmap_sync_v1.ksy`
  - `docs/cvmmap_control_v1.ksy`
  - `docs/cvmmap_shm_metadata_v1_v2.ksy`
  - `docs/cvmmap_body_tracking_v1.ksy`

  This schema freezes the ABI for:
  - ZMQ PUB/SUB sync + module status messages,
  - ZMQ PUB/SUB ZED body tracking messages,
  - ZMQ REQ/REP control messages,
  - POSIX SHM metadata region at bytes [0, 256).

  Contract-level assumptions (normative):
  - Endianness is LITTLE-ENDIAN for all integer fields.
  - Layouts are explicit and PACKED as declared here; consumers must not infer
    compiler/native padding.
  - SHM payload bytes start at offset 256 (`SHM_PAYLOAD_OFFSET`).
  - `CV_MMAP_MAGIC` is the 8-byte ASCII sequence `CV-MMAP\0`.

enums:
  topic_magic:
    125: frame_sync        # 0x7D
    90: module_status      # 0x5A
    60: control_request    # 0x3C
    61: control_response   # 0x3D
    98: body_tracking      # 0x62

  status_code:
    161: online            # 0xA1
    160: offline           # 0xA0
    176: stream_reset      # 0xB0

  pixel_format:
    0: rgb
    1: bgr
    2: rgba
    3: bgra
    4: gray
    5: yuv
    6: yuyv

  depth:
    0: u8
    1: s8
    2: u16
    3: s16
    4: s32
    5: f32
    6: f64
    7: f16

  frame_plane_type:
    0: left
    1: depth
    2: confidence

  body_tracking_model:
    0: human_body_fast
    1: human_body_medium
    2: human_body_accurate

  body_format:
    0: body_18
    1: body_34
    2: body_38

  body_keypoint_selection:
    0: full
    1: upper_body

  inference_precision:
    0: fp32
    1: fp16
    2: int8

  object_tracking_state:
    0: off
    1: ok
    2: searching
    3: terminate

  object_action_state:
    0: idle
    1: moving

types:
  sync_message:
    doc: |
      ZMQ PUB/SUB frame notification message (48 bytes).
      Mirrors `app::sync_message_t`.
    seq:
      - id: magic
        type: u1
        enum: topic_magic
        valid: topic_magic::frame_sync
      - id: reserved_0
        type: u1
        doc: padding byte
      - id: versions_major
        type: u1
      - id: versions_minor
        type: u1
      - id: frame_count
        type: u4
      - id: reserved_1
        size: 4
      - id: reserved_1_padding
        size: 4
        doc: Explicitly serialized reserved bytes (total reserved_1 = 8 bytes)
      - id: timestamp_ns
        type: u8
        doc: Nanoseconds since epoch
      - id: label
        type: strz
        size: 24
        encoding: ASCII

  module_status_message:
    doc: |
      ZMQ PUB/SUB module status notification (32 bytes).
      Mirrors `app::module_status_message_t`.
    seq:
      - id: magic
        type: u1
        enum: topic_magic
        valid: topic_magic::module_status
      - id: reserved_0
        type: u1
        doc: padding byte
      - id: versions_major
        type: u1
      - id: versions_minor
        type: u1
      - id: module_status
        type: s4
        enum: status_code
      - id: label
        type: strz
        size: 24
        encoding: ASCII

  control_message_request:
    doc: |
      ZMQ REQ/REP control request (base: 36 bytes + payload).
      Mirrors `app::control_message_request_t` flexible trailing message.
    seq:
      - id: magic
        type: u1
        enum: topic_magic
        valid: topic_magic::control_request
      - id: reserved_0
        type: u1
        doc: padding byte
      - id: versions_major
        type: u1
      - id: versions_minor
        type: u1
      - id: command_id
        type: s4
      - id: label
        type: strz
        size: 24
        encoding: ASCII
      - id: request_message_size
        type: u2
      - id: request_message
        size: request_message_size
        if: request_message_size > 0

  control_message_response:
    doc: |
      ZMQ REQ/REP control response (base: 40 bytes + payload).
      Mirrors `app::control_message_response_t` flexible trailing message.
    seq:
      - id: magic
        type: u1
        enum: topic_magic
        valid: topic_magic::control_response
      - id: reserved_0
        type: u1
        doc: padding byte
      - id: versions_major
        type: u1
      - id: versions_minor
        type: u1
      - id: command_id
        type: s4
      - id: label
        type: strz
        size: 24
        encoding: ASCII
      - id: response_code
        type: s4
      - id: response_message_size
        type: u2
      - id: response_message
        size: response_message_size
        if: response_message_size > 0

  body_tracking_message:
    doc: |
      ZMQ PUB/SUB body tracking message (64-byte header + fixed-size body records).
      Produced only by the ZED backend when `[zed.body_tracking].enabled=true`.
    seq:
      - id: header
        type: body_tracking_message_header
      - id: bodies
        type: body_tracking_body
        repeat: expr
        repeat-expr: header.body_count

  body_tracking_message_header:
    doc: |
      Fixed-size body tracking header (64 bytes, packed).
      Total packet size is `64 + body_count * body_record_size`.
    seq:
      - id: magic
        type: u1
        enum: topic_magic
        valid: topic_magic::body_tracking
      - id: reserved_0
        type: u1
      - id: versions_major
        type: u1
        valid: _ == 1
      - id: versions_minor
        type: u1
        valid: _ == 0
      - id: frame_count
        type: u4
      - id: timestamp_ns
        type: u8
      - id: sdk_timestamp_ns
        type: u8
      - id: body_count
        type: u2
      - id: body_record_size
        type: u2
        valid: _ == 3248
      - id: body_format
        type: u1
        enum: body_format
      - id: body_selection
        type: u1
        enum: body_keypoint_selection
      - id: detection_model
        type: u1
        enum: body_tracking_model
      - id: inference_precision
        type: u1
        enum: inference_precision
      - id: flags
        type: u2
      - id: reserved_1
        type: u2
      - id: payload_size_bytes
        type: u4
      - id: label
        type: strz
        size: 24
        encoding: ASCII
    instances:
      payload_size_valid:
        value: payload_size_bytes == (body_count * body_record_size)

  body_tracking_body:
    doc: |
      Fixed-size body record (3248 bytes).
      Unavailable scalar/vector values are encoded as IEEE-754 NaN.
      All keypoint arrays are padded to capacity 38 to keep a stable ABI across
      BODY_18, BODY_34, BODY_38, and UPPER_BODY selections.
    seq:
      - id: id
        type: s4
      - id: tracking_state
        type: u1
        enum: object_tracking_state
      - id: action_state
        type: u1
        enum: object_action_state
      - id: reserved_0
        size: 2
      - id: confidence
        type: f4
      - id: position
        type: f4
        repeat: expr
        repeat-expr: 3
      - id: velocity
        type: f4
        repeat: expr
        repeat-expr: 3
      - id: position_covariance
        type: f4
        repeat: expr
        repeat-expr: 6
      - id: bounding_box_2d
        type: vec2_f32
        repeat: expr
        repeat-expr: 4
      - id: bounding_box_3d
        type: vec3_f32
        repeat: expr
        repeat-expr: 8
      - id: dimensions
        type: f4
        repeat: expr
        repeat-expr: 3
      - id: keypoint_2d
        type: vec2_f32
        repeat: expr
        repeat-expr: 38
      - id: keypoint_3d
        type: vec3_f32
        repeat: expr
        repeat-expr: 38
      - id: keypoint_confidence
        type: f4
        repeat: expr
        repeat-expr: 38
      - id: keypoint_covariance
        type: covariance6_f32
        repeat: expr
        repeat-expr: 38
      - id: head_bounding_box_2d
        type: vec2_f32
        repeat: expr
        repeat-expr: 4
      - id: head_bounding_box_3d
        type: vec3_f32
        repeat: expr
        repeat-expr: 8
      - id: head_position
        type: f4
        repeat: expr
        repeat-expr: 3
      - id: local_position_per_joint
        type: vec3_f32
        repeat: expr
        repeat-expr: 38
      - id: local_orientation_per_joint
        type: vec4_f32
        repeat: expr
        repeat-expr: 38
      - id: global_root_orientation
        type: f4
        repeat: expr
        repeat-expr: 4
      - id: keypoint_count
        type: u2
      - id: flags
        type: u2

  vec2_f32:
    seq:
      - id: x
        type: f4
      - id: y
        type: f4

  vec3_f32:
    seq:
      - id: x
        type: f4
      - id: y
        type: f4
      - id: z
        type: f4

  vec4_f32:
    seq:
      - id: x
        type: f4
      - id: y
        type: f4
      - id: z
        type: f4
      - id: w
        type: f4

  covariance6_f32:
    seq:
      - id: values
        type: f4
        repeat: expr
        repeat-expr: 6

  frame_info:
    doc: |
      v1 frame information (12 bytes), mirrors `app::frame_info_t` exactly.
      Field order/size are explicit ABI contract fields.
    seq:
      - id: width
        type: u2
        valid: _ > 0
      - id: height
        type: u2
        valid: _ > 0
      - id: channels
        type: u1
        valid: _ > 0
      - id: depth
        type: u1
        enum: depth
      - id: pixel_format
        type: u1
        enum: pixel_format
      - id: reserved_0
        type: u1
        doc: Explicit reserved byte
      - id: buffer_size
        type: u4
        valid: _ > 0

  frame_metadata:
    doc: |
      Version-dispatched SHM metadata region (exactly 256 bytes).

      Dispatch rules:
      - Read major version at offset 8.
      - major==1 => parse as `frame_metadata_v1`.
      - major==2 => parse as `frame_metadata_v2`.
      - Any other major is ABI-incompatible and must be rejected.
    seq:
      - id: magic
        contents: [67, 86, 45, 77, 77, 65, 80, 0] # "CV-MMAP\0"
      - id: versions_major
        type: u1
        valid: _ == 1 or _ == 2
      - id: versions_minor
        type: u1
      - id: body
        size: 246
    instances:
      as_v1:
        pos: 0
        type: frame_metadata_v1
        if: versions_major == 1
      as_v2:
        pos: 0
        type: frame_metadata_v2
        if: versions_major == 2

  frame_metadata_v1:
    doc: |
      Legacy SHM metadata contract (major=1), fixed 256-byte region.

      Binary layout is the v1 producer contract:
      - [0..39]  : metadata header (`app::frame_metadata_t`, naturally 8-byte aligned)
      - [40..255]: reserved/padding bytes before payload start at offset 256.

      v1 -> normalized synthetic single-plane guidance (normative for clients):
      - normalized.plane_count = 1
      - normalized.plane[0].plane_type = left
      - normalized.plane[0].pixel_format = info.pixel_format
      - normalized.plane[0].depth = info.depth
      - normalized.plane[0].width = info.width
      - normalized.plane[0].height = info.height
      - normalized.plane[0].offset_bytes = 0
      - normalized.plane[0].size_bytes = info.buffer_size
      - normalized.plane[0].stride_bytes =
          info.buffer_size / info.height, only when divisible.
      Reject malformed v1 metadata if normalization_valid is false.
    seq:
      - id: magic
        contents: [67, 86, 45, 77, 77, 65, 80, 0] # "CV-MMAP\0"
      - id: versions_major
        type: u1
        valid: _ == 1
      - id: versions_minor
        type: u1
      - id: reserved_0
        type: u2
      - id: frame_count
        type: u4
      - id: timestamp_ns
        type: u8
      - id: info
        type: frame_info
      - id: reserved_tail_alignment
        type: u4
        doc: Explicit bytes occupying native tail alignment gap in v1 C++ struct.
      - id: trailing_padding_to_payload
        size: 216
        doc: Reserved bytes in [40..255]; payload starts at byte 256.
    instances:
      depth_size_bytes:
        value: 'info.depth == depth::u8 ? 1 : info.depth == depth::s8 ? 1 : info.depth == depth::u16 ? 2 : info.depth == depth::s16 ? 2 : info.depth == depth::s32 ? 4 : info.depth == depth::f32 ? 4 : info.depth == depth::f64 ? 8 : 2'
      expected_min_stride_bytes:
        value: info.width * info.channels * depth_size_bytes
      normalized_stride_bytes:
        value: 'info.height > 0 and (info.buffer_size % info.height) == 0 ? (info.buffer_size / info.height) : 0'
      normalization_valid:
        value: info.height > 0 and normalized_stride_bytes > 0 and normalized_stride_bytes >= expected_min_stride_bytes and (normalized_stride_bytes * info.height) == info.buffer_size

  frame_plane_descriptor_v2:
    doc: |
      v2 fixed-size plane descriptor (24 bytes, packed; no implicit padding).

      Descriptor invariants (normative):
      - `offset_bytes <= payload_size_bytes`
      - `size_bytes <= payload_size_bytes - offset_bytes`
      - Active descriptors must have non-zero width/height/stride/size.
      - Inactive descriptors must be all-zero for geometric + byte-range fields.
    seq:
      - id: plane_type
        type: u1
        enum: frame_plane_type
      - id: pixel_format
        type: u1
        enum: pixel_format
      - id: depth
        type: u1
        enum: depth
      - id: reserved_0
        type: u1
        doc: Explicit reserved byte to keep descriptor size fixed at 24 bytes.
      - id: width
        type: u4
      - id: height
        type: u4
      - id: stride_bytes
        type: u4
      - id: offset_bytes
        type: u4
      - id: size_bytes
        type: u4
    instances:
      is_empty_descriptor:
        value: width == 0 and height == 0 and stride_bytes == 0 and offset_bytes == 0 and size_bytes == 0

  frame_metadata_v2_header:
    doc: |
      v2 metadata header (64 bytes, packed; no implicit padding).

      Offsets are normative:
      - 0x00 magic[8]            = "CV-MMAP\0"
      - 0x08 versions_major      = 2
      - 0x09 versions_minor
      - 0x0A flags               (u16, currently reserved)
      - 0x0C frame_id            (u32)
      - 0x10 capture_ts_ns       (u64)
      - 0x18 publish_seq         (u64)
      - 0x20 plane_count         (u8, 1..4)
      - 0x21 plane_presence_mask (u8)
      - 0x22 plane_desc_offset   (u16, must be 64)
      - 0x24 plane_desc_size     (u16, must be 24)
      - 0x26 plane_desc_capacity (u16, must be 4)
      - 0x28 payload_size_bytes  (u32)
      - 0x2C reserved_0[20]
    seq:
      - id: magic
        contents: [67, 86, 45, 77, 77, 65, 80, 0] # "CV-MMAP\0"
      - id: versions_major
        type: u1
        valid: _ == 2
      - id: versions_minor
        type: u1
      - id: flags
        type: u2
      - id: frame_id
        type: u4
      - id: capture_ts_ns
        type: u8
      - id: publish_seq
        type: u8
      - id: plane_count
        type: u1
        valid: _ >= 1 and _ <= 4
      - id: plane_presence_mask
        type: u1
        valid: (_ & 0xF0) == 0
      - id: plane_descriptors_offset
        type: u2
        valid: _ == 64
      - id: plane_descriptor_size
        type: u2
        valid: _ == 24
      - id: plane_descriptor_capacity
        type: u2
        valid: _ == 4
      - id: payload_size_bytes
        type: u4
        valid: _ > 0
      - id: reserved_0
        size: 20
    instances:
      contiguous_mask_expected:
        value: (1 << plane_count) - 1
      contiguous_mask_valid:
        value: plane_presence_mask == contiguous_mask_expected

  frame_metadata_v2:
    doc: |
      v2 SHM metadata contract (major=2), fixed 256-byte region.

      Layout:
      - [0..63]   : `frame_metadata_v2_header`
      - [64..159] : 4 x `frame_plane_descriptor_v2` (fixed slots)
      - [160..255]: reserved/padding

      Deterministic plane ordering (normative):
      - Active descriptors are contiguous from slot 0.
      - Slot 0 is always LEFT plane.
      - Slot 1 is DEPTH plane when `plane_count >= 2`.
      - Slot 2 is CONFIDENCE plane when `plane_count >= 3`.
      - Slots >= plane_count are inactive and must be empty descriptors.
      - Active planes are packed in payload order: each next offset equals the
        previous offset + previous size.
    seq:
      - id: header
        type: frame_metadata_v2_header
      - id: plane_0
        type: frame_plane_descriptor_v2
      - id: plane_1
        type: frame_plane_descriptor_v2
      - id: plane_2
        type: frame_plane_descriptor_v2
      - id: plane_3
        type: frame_plane_descriptor_v2
      - id: trailing_padding_to_payload
        size: 96
        doc: Reserved bytes in [160..255]; payload starts at byte 256.
    instances:
      plane_0_in_bounds:
        value: plane_0.offset_bytes <= header.payload_size_bytes and plane_0.size_bytes <= (header.payload_size_bytes - plane_0.offset_bytes)
      plane_1_in_bounds:
        value: plane_1.offset_bytes <= header.payload_size_bytes and plane_1.size_bytes <= (header.payload_size_bytes - plane_1.offset_bytes)
      plane_2_in_bounds:
        value: plane_2.offset_bytes <= header.payload_size_bytes and plane_2.size_bytes <= (header.payload_size_bytes - plane_2.offset_bytes)
      plane_3_in_bounds:
        value: plane_3.offset_bytes <= header.payload_size_bytes and plane_3.size_bytes <= (header.payload_size_bytes - plane_3.offset_bytes)
      plane_0_expected_active:
        value: header.plane_count >= 1 and plane_0.is_empty_descriptor == false and plane_0.offset_bytes == 0
      plane_0_type_valid:
        value: 'plane_0.plane_type == frame_plane_type::left'
      plane_0_nonzero_when_active:
        value: 'header.plane_count < 1 ? true : (plane_0.width > 0 and plane_0.height > 0 and plane_0.stride_bytes > 0 and plane_0.size_bytes > 0)'
      plane_1_state_valid:
        value: 'header.plane_count < 2 ? plane_1.is_empty_descriptor : (plane_1.is_empty_descriptor == false and plane_1.offset_bytes == (plane_0.offset_bytes + plane_0.size_bytes))'
      plane_1_type_valid:
        value: 'header.plane_count < 2 ? true : plane_1.plane_type == frame_plane_type::depth'
      plane_1_nonzero_when_active:
        value: 'header.plane_count < 2 ? true : (plane_1.width > 0 and plane_1.height > 0 and plane_1.stride_bytes > 0 and plane_1.size_bytes > 0)'
      plane_2_state_valid:
        value: 'header.plane_count < 3 ? plane_2.is_empty_descriptor : (plane_2.is_empty_descriptor == false and plane_2.offset_bytes == (plane_1.offset_bytes + plane_1.size_bytes))'
      plane_2_type_valid:
        value: 'header.plane_count < 3 ? true : plane_2.plane_type == frame_plane_type::confidence'
      plane_2_nonzero_when_active:
        value: 'header.plane_count < 3 ? true : (plane_2.width > 0 and plane_2.height > 0 and plane_2.stride_bytes > 0 and plane_2.size_bytes > 0)'
      plane_3_state_valid:
        value: 'header.plane_count < 4 ? plane_3.is_empty_descriptor : (plane_3.is_empty_descriptor == false and plane_3.offset_bytes == (plane_2.offset_bytes + plane_2.size_bytes))'
      plane_3_nonzero_when_active:
        value: 'header.plane_count < 4 ? true : (plane_3.width > 0 and plane_3.height > 0 and plane_3.stride_bytes > 0 and plane_3.size_bytes > 0)'
