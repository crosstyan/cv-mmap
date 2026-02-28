meta:
  id: cvmmap
  title: cv-mmap IPC Binary Formats
  endian: le
  license: MIT
doc: |
  Kaitai Struct schemas for the cv-mmap IPC project.
  Includes definitions for ZeroMQ control messages, PUB/SUB notifications,
  and POSIX shared memory layouts.

enums:
  topic_magic:
    125: frame_sync        # 0x7D
    90: module_status      # 0x5A
    115: control_request   # 0x73
    116: control_response  # 0x74

  status_code:
    161: online            # 0xA1
    160: offline           # 0xA0
    176: stream_reset      # 0xB0

types:
  sync_message:
    doc: ZMQ PUB/SUB frame notification message (48 bytes)
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
      - id: padding
        size: 4
        doc: Compiler padding for 8-byte alignment
      - id: timestamp_ns
        type: u8
        doc: Nanoseconds since epoch
      - id: label
        type: strz
        size: 24
        encoding: ASCII

  module_status_message:
    doc: ZMQ PUB/SUB module status notification (32 bytes)
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
    doc: ZMQ REQ/REP control request (Variable size)
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
    doc: ZMQ REQ/REP control response (Variable size)
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

  frame_info:
    doc: Embedded inside shared memory metadata (12 bytes)
    seq:
      - id: width
        type: s4
      - id: height
        type: s4
      - id: channels
        type: s4
      - id: elem_size
        type: s4
        doc: Bytes per pixel (e.g. 1 for CV_8U, 4 for CV_32F)

  frame_metadata:
    doc: POSIX Shared memory metadata region (256 bytes total)
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
      - id: frame_info
        type: frame_info
      - id: timestamp_ns
        type: u8
      - id: reserved_1
        size: 4
      - id: padding
        size: 4
        doc: Compiler padding for 8-byte alignment
      - id: reserved_2
        size: 220
        doc: Padding to reach 256 byte boundary for pixel data
