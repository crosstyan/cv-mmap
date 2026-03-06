meta:
  id: cvmmap_sync_v1
  title: cv-mmap Sync PUB Message v1
  endian: le
  license: MIT

doc: |
  Dedicated Kaitai Struct schema for the cv-mmap frame sync PUB message.
  This matches the v1 ZeroMQ sync topic payload.

enums:
  topic_magic:
    125: frame_sync

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
  - id: reserved_1
    size: 4
  - id: reserved_1_padding
    size: 4
  - id: timestamp_ns
    type: u8
  - id: label
    type: strz
    size: 24
    encoding: ASCII
