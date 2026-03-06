meta:
  id: cvmmap_body_tracking_v1
  title: cv-mmap ZED Body Tracking PUB Message v1
  endian: le
  license: MIT

doc: |
  Dedicated Kaitai Struct schema for the cv-mmap ZED body tracking PUB socket.
  Packet layout is:

  - 64-byte fixed header
  - `body_count` fixed-size body records

enums:
  topic_magic:
    98: body_tracking

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

seq:
  - id: header
    type: body_tracking_message_header
  - id: bodies
    type: body_tracking_body
    repeat: expr
    repeat-expr: header.body_count

types:
  body_tracking_message_header:
    seq:
      - id: magic
        type: u1
        enum: topic_magic
      - id: reserved_0
        type: u1
      - id: versions_major
        type: u1
      - id: versions_minor
        type: u1
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

  body_tracking_body:
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
