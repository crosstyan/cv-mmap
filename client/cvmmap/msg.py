import struct
from dataclasses import dataclass
from enum import Enum, auto

# ---------------------------------------------------------------------------
# Magic constants (must match C++ implementation in app_common_models.hpp)
# ---------------------------------------------------------------------------
FRAME_TOPIC_MAGIC = 0x7D
MODULE_STATUS_MAGIC = 0x5A
CONTROL_MESSAGE_REQUEST_MAGIC = 0x3C
CONTROL_MESSAGE_RESPONSE_MAGIC = 0x3D

# Control command IDs
CONTROL_MSG_CMD_GENERIC = 0
CONTROL_MSG_CMD_RESET_FRAME_COUNT = 0x1001

# Control response codes
CONTROL_RESPONSE_OK = 0
CONTROL_RESPONSE_UNKNOWN_CMD = -1
CONTROL_RESPONSE_ERROR = -2
CONTROL_RESPONSE_INVALID_MAGIC = -3
CONTROL_RESPONSE_INVALID_LABEL = -4
CONTROL_RESPONSE_INVALID_VERSION = -5
CONTROL_RESPONSE_INVALID_MSG_SIZE = -6

# Module status codes
MODULE_STATUS_ONLINE = 0xA1
MODULE_STATUS_OFFLINE = 0xA0
MODULE_STATUS_STREAM_RESET = 0xB0

# Version constants
VERSION_MAJOR = 1
VERSION_MINOR = 0

# Label length constant
LABEL_LEN_MAX = 24

# Shared memory metadata magic constant
# "CV-MMAP\0" exactly 8 bytes – see `frame_metadata_t::CV_MMAP_MAGIC` in C++
CV_MMAP_MAGIC: bytes = b"CV-MMAP\0"
CV_MMAP_MAGIC_LEN: int = len(CV_MMAP_MAGIC)


class PixelFormat(Enum):
    RGB = 0
    BGR = auto()
    RGBA = auto()
    BGRA = auto()
    GRAY = auto()
    YUV = auto()
    YUYV = auto()


class Depth(Enum):
    U8 = 0
    S8 = auto()
    U16 = auto()
    S16 = auto()
    S32 = auto()
    F32 = auto()
    F64 = auto()
    F16 = auto()


# ---------------------------------------------------------------------------
# SyncMessage - matches C++ sync_message_t
# ---------------------------------------------------------------------------
@dataclass
class SyncMessage:
    """
    Sync message sent over ZMQ to notify subscribers of a new frame.

    C++ layout (48 bytes, 8-byte aligned):
        uint8_t _magic            (1 byte)
        uint8_t _reserved_0[1]    (1 byte padding)
        uint8_t versions_major    (1 byte)
        uint8_t versions_minor    (1 byte)
        uint32_t frame_count      (4 bytes)
        uint8_t _reserved_1[4]    (4 bytes padding)
        uint64_t timestamp_ns     (8 bytes)
        uint8_t _label[24]        (24 bytes)
    """

    label: str
    frame_count: int
    timestamp_ns: int = 0
    versions_major: int = VERSION_MAJOR
    versions_minor: int = VERSION_MINOR

    # Pack format: magic(B) + reserved(B) + major(B) + minor(B) + frame_count(I) +
    #              reserved[4](4s) + timestamp_ns(Q) + label(24s)
    PACK_FMT = f"=BBBBI4sQ{LABEL_LEN_MAX}s"

    @staticmethod
    def size() -> int:
        return struct.calcsize(SyncMessage.PACK_FMT)

    def marshal(self) -> bytes:
        """Marshal the SyncMessage to bytes matching the C++ format"""
        if len(self.label) > LABEL_LEN_MAX:
            raise ValueError(f"Label too long: {len(self.label)} > {LABEL_LEN_MAX}")

        label_bytes = self.label.encode("utf-8")[:LABEL_LEN_MAX]
        label_padded = label_bytes + b"\0" * (LABEL_LEN_MAX - len(label_bytes))

        return struct.pack(
            self.PACK_FMT,
            FRAME_TOPIC_MAGIC,
            0,  # reserved_0
            self.versions_major,
            self.versions_minor,
            self.frame_count,
            b"\0\0\0\0",  # reserved_1
            self.timestamp_ns,
            label_padded,
        )

    @staticmethod
    def unmarshal(data: bytes) -> "SyncMessage":
        """Unmarshal bytes to SyncMessage"""
        if len(data) < SyncMessage.size():
            raise ValueError(f"Data too short: {len(data)} < {SyncMessage.size()}")

        (
            magic,
            _reserved_0,
            versions_major,
            versions_minor,
            frame_count,
            _reserved_1,
            timestamp_ns,
            label_raw,
        ) = struct.unpack(SyncMessage.PACK_FMT, data[: SyncMessage.size()])

        if magic != FRAME_TOPIC_MAGIC:
            raise ValueError(
                f"Invalid topic magic: expected {FRAME_TOPIC_MAGIC:#x}, got {magic:#x}"
            )

        label = label_raw.split(b"\0", 1)[0].decode("utf-8")
        return SyncMessage(
            label=label,
            frame_count=frame_count,
            timestamp_ns=timestamp_ns,
            versions_major=versions_major,
            versions_minor=versions_minor,
        )


# ---------------------------------------------------------------------------
# ModuleStatusMessage - matches C++ module_status_message_t
# ---------------------------------------------------------------------------
@dataclass
class ModuleStatusMessage:
    """
    Module status message sent over ZMQ to notify subscribers of module status changes.

    C++ layout (32 bytes):
        uint8_t _magic            (1 byte)
        uint8_t _reserved_0[1]    (1 byte padding)
        uint8_t versions_major    (1 byte)
        uint8_t versions_minor    (1 byte)
        int32_t module_status     (4 bytes)
        uint8_t _label[24]        (24 bytes)
    """

    label: str
    module_status: int
    versions_major: int = VERSION_MAJOR
    versions_minor: int = VERSION_MINOR

    # Pack format: magic(B) + reserved(B) + major(B) + minor(B) + status(i) + label(24s)
    PACK_FMT = f"=BBBBi{LABEL_LEN_MAX}s"

    @staticmethod
    def size() -> int:
        return struct.calcsize(ModuleStatusMessage.PACK_FMT)

    @staticmethod
    def make_online(label: str) -> "ModuleStatusMessage":
        return ModuleStatusMessage(label=label, module_status=MODULE_STATUS_ONLINE)

    @staticmethod
    def make_offline(label: str) -> "ModuleStatusMessage":
        return ModuleStatusMessage(label=label, module_status=MODULE_STATUS_OFFLINE)

    @staticmethod
    def make_frame_reset(label: str) -> "ModuleStatusMessage":
        return ModuleStatusMessage(
            label=label, module_status=MODULE_STATUS_STREAM_RESET
        )

    def marshal(self) -> bytes:
        """Marshal the ModuleStatusMessage to bytes matching the C++ format"""
        if len(self.label) > LABEL_LEN_MAX:
            raise ValueError(f"Label too long: {len(self.label)} > {LABEL_LEN_MAX}")

        label_bytes = self.label.encode("utf-8")[:LABEL_LEN_MAX]
        label_padded = label_bytes + b"\0" * (LABEL_LEN_MAX - len(label_bytes))

        return struct.pack(
            self.PACK_FMT,
            MODULE_STATUS_MAGIC,
            0,  # reserved_0
            self.versions_major,
            self.versions_minor,
            self.module_status,
            label_padded,
        )

    @staticmethod
    def unmarshal(data: bytes) -> "ModuleStatusMessage":
        """Unmarshal bytes to ModuleStatusMessage"""
        if len(data) < ModuleStatusMessage.size():
            raise ValueError(
                f"Data too short: {len(data)} < {ModuleStatusMessage.size()}"
            )

        (
            magic,
            _reserved_0,
            versions_major,
            versions_minor,
            module_status,
            label_raw,
        ) = struct.unpack(
            ModuleStatusMessage.PACK_FMT, data[: ModuleStatusMessage.size()]
        )

        if magic != MODULE_STATUS_MAGIC:
            raise ValueError(
                f"Invalid module status magic: expected {MODULE_STATUS_MAGIC:#x}, got {magic:#x}"
            )

        label = label_raw.split(b"\0", 1)[0].decode("utf-8")
        return ModuleStatusMessage(
            label=label,
            module_status=module_status,
            versions_major=versions_major,
            versions_minor=versions_minor,
        )

    def is_online(self) -> bool:
        return self.module_status == MODULE_STATUS_ONLINE

    def is_offline(self) -> bool:
        return self.module_status == MODULE_STATUS_OFFLINE

    def is_stream_reset(self) -> bool:
        return self.module_status == MODULE_STATUS_STREAM_RESET


# ---------------------------------------------------------------------------
# ControlMessageRequest - matches C++ control_message_request_t
# ---------------------------------------------------------------------------
@dataclass
class ControlMessageRequest:
    """
    Control message request sent to server.

    C++ layout (variable size, base 34 bytes):
        uint8_t _magic            (1 byte)
        uint8_t _reserved_0[1]    (1 byte padding)
        uint8_t versions_major    (1 byte)
        uint8_t versions_minor    (1 byte)
        int32_t command_id        (4 bytes)
        uint8_t _label[24]        (24 bytes)
        uint16_t request_message_length (2 bytes)
        uint8_t _request_message_data[] (variable)
    """

    label: str
    command_id: int
    request_message: bytes = b""
    versions_major: int = VERSION_MAJOR
    versions_minor: int = VERSION_MINOR

    # Pack format for header (without flexible array member)
    # magic(B) + reserved(B) + major(B) + minor(B) + command_id(i) + label(24s) + msg_len(H)
    HEADER_FMT = f"=BBBBi{LABEL_LEN_MAX}sH"

    @staticmethod
    def header_size() -> int:
        return struct.calcsize(ControlMessageRequest.HEADER_FMT)

    def size(self) -> int:
        return self.header_size() + len(self.request_message)

    def marshal(self) -> bytes:
        """Marshal the ControlMessageRequest to bytes matching the C++ format"""
        if len(self.label) > LABEL_LEN_MAX:
            raise ValueError(f"Label too long: {len(self.label)} > {LABEL_LEN_MAX}")

        label_bytes = self.label.encode("utf-8")[:LABEL_LEN_MAX]
        label_padded = label_bytes + b"\0" * (LABEL_LEN_MAX - len(label_bytes))

        header = struct.pack(
            self.HEADER_FMT,
            CONTROL_MESSAGE_REQUEST_MAGIC,
            0,  # reserved_0
            self.versions_major,
            self.versions_minor,
            self.command_id,
            label_padded,
            len(self.request_message),
        )
        return header + self.request_message

    @staticmethod
    def unmarshal(data: bytes) -> "ControlMessageRequest":
        """Unmarshal bytes to ControlMessageRequest"""
        header_size = ControlMessageRequest.header_size()
        if len(data) < header_size:
            raise ValueError(f"Data too short: {len(data)} < {header_size}")

        (
            magic,
            _reserved_0,
            versions_major,
            versions_minor,
            command_id,
            label_raw,
            msg_len,
        ) = struct.unpack(ControlMessageRequest.HEADER_FMT, data[:header_size])

        if magic != CONTROL_MESSAGE_REQUEST_MAGIC:
            raise ValueError(
                f"Invalid request magic: expected {CONTROL_MESSAGE_REQUEST_MAGIC:#x}, got {magic:#x}"
            )

        label = label_raw.split(b"\0", 1)[0].decode("utf-8")
        request_message = data[header_size : header_size + msg_len]

        return ControlMessageRequest(
            label=label,
            command_id=command_id,
            request_message=request_message,
            versions_major=versions_major,
            versions_minor=versions_minor,
        )


# ---------------------------------------------------------------------------
# ControlMessageResponse - matches C++ control_message_response_t
# ---------------------------------------------------------------------------
@dataclass
class ControlMessageResponse:
    """
    Control message response from server.

    C++ layout (variable size, base 38 bytes):
        uint8_t _magic            (1 byte)
        uint8_t _reserved_0[1]    (1 byte padding)
        uint8_t versions_major    (1 byte)
        uint8_t versions_minor    (1 byte)
        int32_t command_id        (4 bytes)
        int32_t response_code     (4 bytes)
        uint8_t _label[24]        (24 bytes)
        uint16_t response_message_length (2 bytes)
        uint8_t _response_message_data[] (variable)
    """

    label: str
    command_id: int
    response_code: int
    response_message: bytes = b""
    versions_major: int = VERSION_MAJOR
    versions_minor: int = VERSION_MINOR

    # Pack format for header (without flexible array member)
    # magic(B) + reserved(B) + major(B) + minor(B) + command_id(i) + response_code(i) + label(24s) + msg_len(H)
    HEADER_FMT = f"=BBBBii{LABEL_LEN_MAX}sH"

    @staticmethod
    def header_size() -> int:
        return struct.calcsize(ControlMessageResponse.HEADER_FMT)

    def size(self) -> int:
        return self.header_size() + len(self.response_message)

    def marshal(self) -> bytes:
        """Marshal the ControlMessageResponse to bytes matching the C++ format"""
        if len(self.label) > LABEL_LEN_MAX:
            raise ValueError(f"Label too long: {len(self.label)} > {LABEL_LEN_MAX}")

        label_bytes = self.label.encode("utf-8")[:LABEL_LEN_MAX]
        label_padded = label_bytes + b"\0" * (LABEL_LEN_MAX - len(label_bytes))

        header = struct.pack(
            self.HEADER_FMT,
            CONTROL_MESSAGE_RESPONSE_MAGIC,
            0,  # reserved_0
            self.versions_major,
            self.versions_minor,
            self.command_id,
            self.response_code,
            label_padded,
            len(self.response_message),
        )
        return header + self.response_message

    @staticmethod
    def unmarshal(data: bytes) -> "ControlMessageResponse":
        """Unmarshal bytes to ControlMessageResponse"""
        header_size = ControlMessageResponse.header_size()
        if len(data) < header_size:
            raise ValueError(f"Data too short: {len(data)} < {header_size}")

        (
            magic,
            _reserved_0,
            versions_major,
            versions_minor,
            command_id,
            response_code,
            label_raw,
            msg_len,
        ) = struct.unpack(ControlMessageResponse.HEADER_FMT, data[:header_size])

        if magic != CONTROL_MESSAGE_RESPONSE_MAGIC:
            raise ValueError(
                f"Invalid response magic: expected {CONTROL_MESSAGE_RESPONSE_MAGIC:#x}, got {magic:#x}"
            )

        label = label_raw.split(b"\0", 1)[0].decode("utf-8")
        response_message = data[header_size : header_size + msg_len]

        return ControlMessageResponse(
            label=label,
            command_id=command_id,
            response_code=response_code,
            response_message=response_message,
            versions_major=versions_major,
            versions_minor=versions_minor,
        )

    def is_ok(self) -> bool:
        return self.response_code == CONTROL_RESPONSE_OK


# ---------------------------------------------------------------------------
# FrameInfo - matches C++ frame_info_t
# ---------------------------------------------------------------------------
@dataclass
class FrameInfo:
    """
    Frame information structure.

    C++ layout (12 bytes, 4-byte aligned):
        uint16_t width           (2 bytes)
        uint16_t height          (2 bytes)
        uint8_t channels         (1 byte)
        Depth depth              (1 byte)
        PixelFormat pixel_format (1 byte)
        uint8_t _reserved_0[1]   (1 byte padding)
        uint32_t buffer_size     (4 bytes)
    """

    width: int
    height: int
    channels: int
    depth: Depth
    pixel_format: PixelFormat
    buffer_size: int

    # Pack format: width(H) + height(H) + channels(B) + depth(B) + pixel_format(B) + reserved(B) + buffer_size(I)
    PACK_FMT = "=HHBBBI"

    @staticmethod
    def size() -> int:
        return struct.calcsize(FrameInfo.PACK_FMT)

    def marshal(self) -> bytes:
        """Marshal the FrameInfo to bytes matching the C++ format"""
        return struct.pack(
            self.PACK_FMT,
            self.width,
            self.height,
            self.channels,
            self.depth.value,
            self.pixel_format.value,
            0,  # reserved_0
            self.buffer_size,
        )

    @staticmethod
    def unmarshal(data: bytes) -> "FrameInfo":
        """Unmarshal bytes to FrameInfo"""
        if len(data) < FrameInfo.size():
            raise ValueError(f"Data too short: {len(data)} < {FrameInfo.size()}")

        (
            width,
            height,
            channels,
            depth_raw,
            pixel_format_raw,
            _reserved_0,
            buffer_size,
        ) = struct.unpack(FrameInfo.PACK_FMT, data[: FrameInfo.size()])

        return FrameInfo(
            width=width,
            height=height,
            channels=channels,
            depth=Depth(depth_raw),
            pixel_format=PixelFormat(pixel_format_raw),
            buffer_size=buffer_size,
        )

    def pixel_size(self) -> int:
        """Get pixel size in bytes"""
        depth_sizes = {
            Depth.U8: 1,
            Depth.S8: 1,
            Depth.U16: 2,
            Depth.S16: 2,
            Depth.S32: 4,
            Depth.F32: 4,
            Depth.F64: 8,
            Depth.F16: 2,
        }
        return depth_sizes.get(self.depth, 1) * self.channels


# ---------------------------------------------------------------------------
# FrameMetadata - matches C++ frame_metadata_t
# ---------------------------------------------------------------------------
@dataclass
class FrameMetadata:
    """
    Frame metadata stored in shared memory.

    C++ layout (8-byte aligned):
        uint8_t magic[8]         (8 bytes, "CV-MMAP\0")
        uint8_t versions_major   (1 byte)
        uint8_t versions_minor   (1 byte)
        uint8_t _reserved_0[1]   (1 byte, but actually 4-3=1 in C++)
        uint32_t frame_count     (4 bytes)
        uint64_t timestamp_ns    (8 bytes)
        frame_info_t info        (12 bytes)

    Note: The C++ code has `uint8_t _reserved_0[4 - 3]` which equals 1 byte,
    but due to alignment for uint32_t frame_count, there might be padding.
    Looking at the structure, after versions (2 bytes) + reserved (1 byte) = 3 bytes,
    we need 1 more byte for 4-byte alignment before frame_count.
    """

    frame_count: int
    timestamp_ns: int
    info: FrameInfo
    versions_major: int = VERSION_MAJOR
    versions_minor: int = VERSION_MINOR

    # Pack format (after 8-byte magic):
    # major(B) + minor(B) + reserved(B) + padding(B) + frame_count(I) + timestamp_ns(Q) + FrameInfo
    # Note: Need 2 bytes of padding after versions to align frame_count to 4-byte boundary
    HEADER_FMT = "=BBBBI"  # major, minor, reserved, padding, frame_count
    TIMESTAMP_FMT = "=Q"  # timestamp_ns

    @staticmethod
    def size() -> int:
        # magic(8) + versions(2) + reserved(2 for alignment) + frame_count(4) + timestamp_ns(8) + FrameInfo
        return CV_MMAP_MAGIC_LEN + 4 + 4 + 8 + FrameInfo.size()

    @staticmethod
    def unmarshal(data: bytes) -> "FrameMetadata":
        """
        Unmarshal bytes to FrameMetadata.

        Note: This expects data starting AFTER the magic bytes (offset from CV_MMAP_MAGIC_LEN).
        The caller should validate the magic and pass the remaining data.
        """
        # Parse: major(B) + minor(B) + reserved[2](2B for alignment) + frame_count(I) + timestamp_ns(Q)
        header_fmt = "=BBBBI"
        header_size = struct.calcsize(header_fmt)

        if len(data) < header_size:
            raise ValueError(f"Data too short for header: {len(data)} < {header_size}")

        (
            versions_major,
            versions_minor,
            _reserved_0,
            _padding,
            frame_count,
        ) = struct.unpack(header_fmt, data[:header_size])

        # Parse timestamp_ns
        timestamp_offset = header_size
        timestamp_size = struct.calcsize("=Q")
        timestamp_ns = struct.unpack(
            "=Q", data[timestamp_offset : timestamp_offset + timestamp_size]
        )[0]

        # Parse FrameInfo
        info_offset = timestamp_offset + timestamp_size
        info = FrameInfo.unmarshal(data[info_offset:])

        return FrameMetadata(
            frame_count=frame_count,
            timestamp_ns=timestamp_ns,
            info=info,
            versions_major=versions_major,
            versions_minor=versions_minor,
        )
