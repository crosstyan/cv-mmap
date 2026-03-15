meta:
  id: cvmmap_control_v1
  title: cv-mmap Control REQ/REP Messages v1
  endian: le
  license: MIT

doc: |
  Dedicated Kaitai Struct schema for cv-mmap control request/response packets.

  Important wire-layout note:

  - the C++ request struct is `sizeof(...) == 36`, but the on-wire request
    header is 34 bytes
  - the C++ response struct is `sizeof(...) == 40`, but the on-wire response
    header is 38 bytes

  The difference is trailing ABI padding after the final `u2` flexible-array
  length field. ZeroMQ packets do not include that tail padding. This schema
  models the actual on-wire byte layout.

enums:
  control_message_magic:
    60: request
    61: response

  control_command:
    0: generic
    4097: reset_frame_count
    4098: get_source_info
    4099: seek_timestamp_ns

  control_response_code:
    0: ok
    -1: unknown_cmd
    -2: error
    -3: invalid_magic
    -4: invalid_label
    -5: invalid_version
    -6: invalid_msg_size
    -7: unsupported
    -8: invalid_payload
    -9: out_of_range

  source_kind:
    0: unknown
    1: live
    2: finite

  timestamp_domain:
    0: unknown
    1: unix_epoch_ns
    2: media_time_ns

types:
  control_message_request:
    doc: |
      Control REQ packet.

      Fixed wire header size is 34 bytes:

      - 4-byte version/magic prefix
      - 4-byte command id
      - 24-byte ASCII label
      - 2-byte payload length
    seq:
      - id: magic
        type: u1
        enum: control_message_magic
      - id: reserved_0
        type: u1
      - id: versions_major
        type: u1
      - id: versions_minor
        type: u1
      - id: command_id
        type: s4
        enum: control_command
      - id: label
        type: strz
        size: 24
        encoding: ASCII
      - id: len_request_message_raw
        type: u2
      - id: request_message_raw
        size: len_request_message_raw
        if: len_request_message_raw > 0
    instances:
      header_size:
        value: 34
      total_size:
        value: header_size + len_request_message_raw
      request_message:
        pos: 34
        size: len_request_message_raw
        type:
          switch-on: command_id
          cases:
            'control_command::seek_timestamp_ns': seek_timestamp_request_v1
        if: len_request_message_raw > 0 and command_id == control_command::seek_timestamp_ns

  control_message_response:
    doc: |
      Control REP packet.

      Fixed wire header size is 38 bytes:

      - 4-byte version/magic prefix
      - 4-byte command id
      - 4-byte response code
      - 24-byte ASCII label
      - 2-byte payload length
    seq:
      - id: magic
        type: u1
        enum: control_message_magic
      - id: reserved_0
        type: u1
      - id: versions_major
        type: u1
      - id: versions_minor
        type: u1
      - id: command_id
        type: s4
        enum: control_command
      - id: response_code
        type: s4
        enum: control_response_code
      - id: label
        type: strz
        size: 24
        encoding: ASCII
      - id: len_response_message_raw
        type: u2
      - id: response_message_raw
        size: len_response_message_raw
        if: len_response_message_raw > 0
    instances:
      header_size:
        value: 38
      total_size:
        value: header_size + len_response_message_raw
      response_message:
        pos: 38
        size: len_response_message_raw
        type:
          switch-on: command_id
          cases:
            'control_command::get_source_info': source_info_response_v1
            'control_command::seek_timestamp_ns': seek_timestamp_response_v1
        if: len_response_message_raw > 0 and response_code == control_response_code::ok and
            (command_id == control_command::get_source_info or
             command_id == control_command::seek_timestamp_ns)

  source_info_response_v1:
    doc: |
      Successful payload for `GET_SOURCE_INFO`.

      `timeline_start_ns`, `timeline_end_ns`, and `duration_ns` are meaningful
      for finite sources. `current_timestamp_ns` always reports the producer's
      current playback/capture position in the declared timestamp domain.
    seq:
      - id: struct_size
        type: u2
      - id: source_kind
        type: u1
        enum: source_kind
      - id: timestamp_domain
        type: u1
        enum: timestamp_domain
      - id: flags
        type: u4
      - id: timeline_start_ns
        type: u8
      - id: timeline_end_ns
        type: u8
      - id: duration_ns
        type: u8
      - id: current_timestamp_ns
        type: u8
      - id: current_frame_count
        type: u4
      - id: reserved_0
        type: u4
    instances:
      expected_wire_size:
        value: 48
      can_seek:
        value: (flags & 0x00000001) != 0
      auto_loop:
        value: (flags & 0x00000002) != 0
      has_depth:
        value: (flags & 0x00000004) != 0
      has_body:
        value: (flags & 0x00000008) != 0

  seek_timestamp_request_v1:
    doc: |
      Request payload for `SEEK_TIMESTAMP_NS`.
    seq:
      - id: struct_size
        type: u2
      - id: reserved_0
        type: u2
      - id: target_timestamp_ns
        type: u8
    instances:
      expected_wire_size:
        value: 12

  seek_timestamp_response_v1:
    doc: |
      Successful payload for `SEEK_TIMESTAMP_NS`.

      The producer lands on the first sample whose timestamp is greater than or
      equal to the requested timestamp. `exact_match` distinguishes exact hits
      from nearest-forward landings.
    seq:
      - id: struct_size
        type: u2
      - id: exact_match
        type: u1
      - id: reserved_0
        type: u1
      - id: requested_timestamp_ns
        type: u8
      - id: landed_timestamp_ns
        type: u8
      - id: landed_frame_count
        type: u4
      - id: reserved_1
        type: u4
    instances:
      expected_wire_size:
        value: 28
