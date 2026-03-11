meta:
  id: cvmmap_shm_metadata_v1_v2
  title: cv-mmap SHM Metadata Region v1/v2
  endian: le
  license: MIT

doc: |
  Dedicated Kaitai Struct schema for the 256-byte shared-memory metadata region.
  This schema dispatches on the metadata major version after the 8-byte magic.

enums:
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

  depth_unit:
    0: unknown
    1: millimeter
    2: meter

seq:
  - id: magic
    contents: [67, 86, 45, 77, 77, 65, 80, 0]
  - id: versions_major
    type: u1
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

types:
  frame_info:
    seq:
      - id: width
        type: u2
      - id: height
        type: u2
      - id: channels
        type: u1
      - id: depth
        type: u1
        enum: depth
      - id: pixel_format
        type: u1
        enum: pixel_format
      - id: reserved_0
        type: u1
      - id: buffer_size
        type: u4

  frame_metadata_v1:
    seq:
      - id: magic
        contents: [67, 86, 45, 77, 77, 65, 80, 0]
      - id: versions_major
        type: u1
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
      - id: trailing_padding_to_payload
        size: 216

  frame_plane_descriptor_v2:
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

  frame_metadata_v2_header:
    seq:
      - id: magic
        contents: [67, 86, 45, 77, 77, 65, 80, 0]
      - id: versions_major
        type: u1
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
      - id: plane_presence_mask
        type: u1
      - id: plane_descriptors_offset
        type: u2
      - id: plane_descriptor_size
        type: u2
      - id: plane_descriptor_capacity
        type: u2
      - id: payload_size_bytes
        type: u4
      - id: depth_unit
        type: u1
        enum: depth_unit
      - id: reserved_0
        size: 19

  frame_metadata_v2:
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
