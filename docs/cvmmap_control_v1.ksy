meta:
  id: cvmmap_control_v1
  title: cv-mmap Control REQ/REP Messages v1
  endian: le
  license: MIT

doc: |
  Dedicated Kaitai Struct schema for cv-mmap control request/response packets.
  Use the appropriate root depending on transport direction.

types:
  control_message_request:
    seq:
      - id: magic
        type: u1
      - id: reserved_0
        type: u1
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
      - id: len_request_message
        type: u2
      - id: request_message
        size: len_request_message
        if: len_request_message > 0

  control_message_response:
    seq:
      - id: magic
        type: u1
      - id: reserved_0
        type: u1
      - id: versions_major
        type: u1
      - id: versions_minor
        type: u1
      - id: command_id
        type: s4
      - id: response_code
        type: s4
      - id: label
        type: strz
        size: 24
        encoding: ASCII
      - id: len_response_message
        type: u2
      - id: response_message
        size: len_response_message
        if: len_response_message > 0
