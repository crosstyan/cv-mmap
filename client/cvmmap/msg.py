import struct
from dataclasses import dataclass
from typing import Optional

# Common constants from app_common_models.hpp
LABEL_LEN_MAX = 24

FRAME_TOPIC_MAGIC = 0x7D
MODULE_STATUS_MAGIC = 0x5A

CONTROL_MESSAGE_REQUEST_MAGIC = 0x3C
CONTROL_MESSAGE_RESPONSE_MAGIC = 0x3D

CONTROL_MSG_CMD_GENERIC = 0
CONTROL_MSG_CMD_RESET_FRAME_COUNT = 0x1001

CONTROL_RESPONSE_OK = 0
CONTROL_RESPONSE_UNKNOWN_CMD = -1
CONTROL_RESPONSE_ERROR = -2
CONTROL_RESPONSE_INVALID_MAGIC = -3
CONTROL_RESPONSE_INVALID_LABEL = -4
CONTROL_RESPONSE_INVALID_VERSION = -5
CONTROL_RESPONSE_INVALID_MSG_SIZE = -6

MODULE_STATUS_ONLINE = 0xA1
MODULE_STATUS_OFFLINE = 0xA0
MODULE_STATUS_STREAM_RESET = 0xB0

VERSION_MAJOR = 1
VERSION_MINOR = 0

# "CV-MMAP\0" exactly 8 bytes – see `frame_metadata_t::CV_MMAP_MAGIC` in C++
CV_MMAP_MAGIC: bytes = b"CV-MMAP\0"
CV_MMAP_MAGIC_LEN: int = len(CV_MMAP_MAGIC)


# SyncMessage - matches C++ sync_message_t
@dataclass
class SyncMessage:
    """
    Sync message sent over ZMQ to notify subscribers of a new frame.
    """

    frame_count: int
    timestamp_ns: int
    label: str

    # C++ struct layout:
    # uint8_t _magic{FRAME_TOPIC_MAGIC};        // offset 0
    # uint8_t _reserved_0[1];                   // offset 1
    # uint8_t versions_major;                   // offset 2
    # uint8_t versions_minor;                   // offset 3
    # uint32_t frame_count;                     // offset 4
    # uint8_t _reserved_1[4];                   // offset 8
    # 4 bytes implicit padding                  // offset 12
    # uint64_t timestamp_ns;                    // offset 16
    # uint8_t _label[LABEL_LEN_MAX];            // offset 24
    # (Total: 48 bytes due to 8-byte alignment)
    PACK_FMT = f"=BxBBI4s4xQ{LABEL_LEN_MAX}s"

    @staticmethod
    def size() -> int:
        return struct.calcsize(SyncMessage.PACK_FMT)

    def marshal(self) -> bytes:
        """Marshal the SyncMessage to bytes matching the C++ format"""
        encoded_label = self.label.encode("utf-8")[:LABEL_LEN_MAX].ljust(
            LABEL_LEN_MAX, b"\0"
        )
        return struct.pack(
            self.PACK_FMT,
            FRAME_TOPIC_MAGIC,
            VERSION_MAJOR,
            VERSION_MINOR,
            self.frame_count,
            b"\0" * 4,
            self.timestamp_ns,
            encoded_label,
        )

    @staticmethod
    def unmarshal(data: bytes) -> "SyncMessage":
        """Unmarshal bytes to SyncMessage"""
        if len(data) < SyncMessage.size():
            raise ValueError(f"Data too short: {len(data)} < {SyncMessage.size()}")

        (
            magic,
            v_major,
            v_minor,
            frame_count,
            _reserved_1,
            timestamp_ns,
            label_bytes,
        ) = struct.unpack(SyncMessage.PACK_FMT, data[: SyncMessage.size()])

        if magic != FRAME_TOPIC_MAGIC:
            raise ValueError(
                f"Invalid topic magic: expected {FRAME_TOPIC_MAGIC:#x}, got {magic:#x}"
            )

        if v_major != VERSION_MAJOR:
            # We only strictly check major version
            pass

        label = label_bytes.split(b"\0", 1)[0].decode("utf-8")

        return SyncMessage(
            frame_count=frame_count, timestamp_ns=timestamp_ns, label=label
        )


@dataclass
class ModuleStatusMessage:
    """
    Module status message sent over ZMQ
    """

    module_status: int
    label: str

    # C++ struct layout:
    # uint8_t _magic{MODULE_STATUS_MAGIC};      // offset 0
    # uint8_t _reserved_0[1];                   // offset 1
    # uint8_t versions_major{VERSION_MAJOR};    // offset 2
    # uint8_t versions_minor{VERSION_MINOR};    // offset 3
    # int32_t module_status;                    // offset 4
    # uint8_t _label[LABEL_LEN_MAX];            // offset 8
    # (Total: 32 bytes)
    PACK_FMT = f"=BxBBi{LABEL_LEN_MAX}s"

    @staticmethod
    def size() -> int:
        return struct.calcsize(ModuleStatusMessage.PACK_FMT)

    def marshal(self) -> bytes:
        encoded_label = self.label.encode("utf-8")[:LABEL_LEN_MAX].ljust(
            LABEL_LEN_MAX, b"\0"
        )
        return struct.pack(
            self.PACK_FMT,
            MODULE_STATUS_MAGIC,
            VERSION_MAJOR,
            VERSION_MINOR,
            self.module_status,
            encoded_label,
        )

    @staticmethod
    def unmarshal(data: bytes) -> "ModuleStatusMessage":
        if len(data) < ModuleStatusMessage.size():
            raise ValueError(
                f"Data too short: {len(data)} < {ModuleStatusMessage.size()}"
            )

        magic, v_major, v_minor, status, label_bytes = struct.unpack(
            ModuleStatusMessage.PACK_FMT, data[: ModuleStatusMessage.size()]
        )

        if magic != MODULE_STATUS_MAGIC:
            raise ValueError(
                f"Invalid module status magic: expected {MODULE_STATUS_MAGIC:#x}, got {magic:#x}"
            )

        label = label_bytes.split(b"\0", 1)[0].decode("utf-8")
        return ModuleStatusMessage(module_status=status, label=label)


@dataclass
class FrameInfo:
    """
    Matches C++ frame_info_t

    struct frame_info_t {
        uint16_t width;
        uint16_t height;
        uint8_t channels;
        Depth depth;  // uint8_t
        PixelFormat pixel_format; // uint8_t
        uint8_t _reserved_0[1]; // padding
        uint32_t buffer_size;
    }
    (Total: 12 bytes, 4-byte aligned)
    """

    width: int
    height: int
    channels: int
    depth: int
    pixel_format: int
    buffer_size: int

    # struct mapping: H = uint16, B = uint8, I = uint32, x = padding byte
    PACK_FMT = "=HHBBBxI"

    @staticmethod
    def size() -> int:
        return struct.calcsize(FrameInfo.PACK_FMT)

    def marshal(self) -> bytes:
        return struct.pack(
            self.PACK_FMT,
            self.width,
            self.height,
            self.channels,
            self.depth,
            self.pixel_format,
            self.buffer_size,
        )

    @staticmethod
    def unmarshal(data: bytes) -> "FrameInfo":
        if len(data) < FrameInfo.size():
            raise ValueError(f"Data too short: {len(data)} < {FrameInfo.size()}")

        (
            width,
            height,
            channels,
            depth,
            pixel_format,
            buffer_size,
        ) = struct.unpack(FrameInfo.PACK_FMT, data[: FrameInfo.size()])

        return FrameInfo(
            width=width,
            height=height,
            channels=channels,
            depth=depth,
            pixel_format=pixel_format,
            buffer_size=buffer_size,
        )


@dataclass
class FrameMetadata:
    """
    Matches C++ frame_metadata_t
    """

    frame_count: int
    timestamp_ns: int
    info: FrameInfo

    @staticmethod
    def size() -> int:
        return CV_MMAP_MAGIC_LEN + struct.calcsize("=BBxxIQ") + FrameInfo.size()

    @staticmethod
    def unmarshal(data: bytes) -> "FrameMetadata":
        """
        Note: This expects data starting AFTER the magic bytes (offset from CV_MMAP_MAGIC_LEN).
        """
        # C++ layout (after magic bytes):
        # uint8_t versions_major;                   // offset 0
        # uint8_t versions_minor;                   // offset 1
        # uint8_t _reserved_0[2];                   // offset 2
        # uint32_t frame_count;                     // offset 4
        # uint64_t timestamp_ns;                    // offset 8
        # frame_info_t info;                        // offset 16
        # (Header without magic + info is 16 + 12 = 28 bytes)
        # Total with 8-byte magic + 4 byte trailing padding (for 8-byte alignment) = 40 bytes

        fmt = "=BBxxIQ"
        header_size = struct.calcsize(fmt)

        if len(data) < header_size + FrameInfo.size():
            raise ValueError("Data too short for FrameMetadata")

        v_major, v_minor, frame_count, timestamp_ns = struct.unpack(
            fmt, data[:header_size]
        )

        info = FrameInfo.unmarshal(data[header_size:])

        return FrameMetadata(
            frame_count=frame_count,
            timestamp_ns=timestamp_ns,
            info=info,
        )


@dataclass
class ControlMessageRequest:
    """
    Request message sent over ZMQ REQ socket
    """

    label: str
    command_id: int
    request_message: bytes

    @staticmethod
    def marshal_format() -> str:
        # uint8_t _magic                            // offset 0
        # uint8_t _reserved_0[1];                   // offset 1
        # uint8_t versions_major                    // offset 2
        # uint8_t versions_minor                    // offset 3
        # int32_t command_id                        // offset 4
        # uint8_t _label[LABEL_LEN_MAX]             // offset 8
        # uint16_t request_message_length;          // offset 32
        # (followed by variable length request message)
        # (Base size: 34 bytes padded to 36 bytes for 4-byte alignment of int32_t)
        return f"=BxBBi{LABEL_LEN_MAX}sH"

    def marshal(self) -> bytes:
        encoded_label = self.label.encode("utf-8")[:LABEL_LEN_MAX].ljust(
            LABEL_LEN_MAX, b"\0"
        )
        msg_len = len(self.request_message)
        header = struct.pack(
            self.marshal_format(),
            CONTROL_MESSAGE_REQUEST_MAGIC,
            VERSION_MAJOR,
            VERSION_MINOR,
            self.command_id,
            encoded_label,
            msg_len
        )
        return header + self.request_message

    @staticmethod
    def header_size() -> int:
        return struct.calcsize(ControlMessageRequest.marshal_format())


@dataclass
class ControlMessageResponse:
    """
    Response message received over ZMQ REP socket
    """

    command_id: int
    response_code: int
    label: str
    response_message: bytes

    @staticmethod
    def marshal_format() -> str:
        # uint8_t _magic                            // offset 0
        # uint8_t _reserved_0[1];                   // offset 1
        # uint8_t versions_major                    // offset 2
        # uint8_t versions_minor                    // offset 3
        # int32_t command_id                        // offset 4
        # int32_t response_code                     // offset 8
        # uint8_t _label[LABEL_LEN_MAX];            // offset 12
        # uint16_t response_message_length;         // offset 36
        # (Base size: 38 bytes padded to 40 bytes)
        return f"=BxBBii{LABEL_LEN_MAX}sH"

    @staticmethod
    def header_size() -> int:
        return struct.calcsize(ControlMessageResponse.marshal_format())

    @staticmethod
    def unmarshal(data: bytes) -> "ControlMessageResponse":
        if len(data) < ControlMessageResponse.header_size():
            raise ValueError(
                f"Data too short: {len(data)} < {ControlMessageResponse.header_size()}"
            )

        (
            magic,
            v_major,
            v_minor,
            command_id,
            response_code,
            label_bytes,
            msg_len
        ) = struct.unpack(
            ControlMessageResponse.marshal_format(),
            data[: ControlMessageResponse.header_size()],
        )

        if magic != CONTROL_MESSAGE_RESPONSE_MAGIC:
            raise ValueError(
                f"Invalid response magic: expected {CONTROL_MESSAGE_RESPONSE_MAGIC:#x}, got {magic:#x}"
            )

        label = label_bytes.split(b"\0", 1)[0].decode("utf-8")
        response_msg = data[ControlMessageResponse.header_size() : ControlMessageResponse.header_size() + msg_len]

        return ControlMessageResponse(
            command_id=command_id,
            response_code=response_code,
            label=label,
            response_message=response_msg,
        )
