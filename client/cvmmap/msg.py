import struct
from dataclasses import dataclass
from enum import Enum, auto

FRAME_TOPIC_MAGIC = 0x7D

# ---------------------------------------------------------------------------
# Shared memory metadata magic constant (must match C++ implementation)
# ---------------------------------------------------------------------------

# "CV-MMAP\0" exactly 8 bytes – see `frame_metadata_t::CV_MMAP_MAGIC` in C++
CV_MMAP_MAGIC: bytes = b"CV-MMAP\0"
# Convenience length constant so we do not sprinkle magic numbers elsewhere
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


@dataclass
class SyncMessage:
    LABEL_LEN = 24

    label: str
    frame_count: int

    def marshal(self) -> bytes:
        """Marshal the SyncMessage to bytes matching the C++ format"""
        # Ensure label fits in the allocated space
        if len(self.label) > SyncMessage.LABEL_LEN:
            raise ValueError(
                f"Label too long: {len(self.label)} > {SyncMessage.LABEL_LEN}"
            )

        # Pad label to LABEL_LEN bytes and null-terminate
        label_bytes = self.label.encode("utf-8")[: SyncMessage.LABEL_LEN]
        label_padded = label_bytes + b"\0" * (SyncMessage.LABEL_LEN - len(label_bytes))

        # Pack: magic (1 byte) + frame_count (4 bytes) + label (24 bytes)
        fmt = f"=BI{SyncMessage.LABEL_LEN}s"
        return struct.pack(fmt, FRAME_TOPIC_MAGIC, self.frame_count, label_padded)

    @staticmethod
    def unmarshal(data: bytes) -> "SyncMessage":
        # Now using the marshal function: magic (1 byte) + frame_count (4 bytes) + label (24 bytes)
        # This matches the C++ struct layout: attr _attribute; label _label;
        fmt = f"=BI{SyncMessage.LABEL_LEN}s"
        magic, frame_count, label_raw = struct.unpack(fmt, data[: struct.calcsize(fmt)])
        assert (
            magic == FRAME_TOPIC_MAGIC
        ), f"Invalid topic magic: expected {FRAME_TOPIC_MAGIC}, got {magic}"
        label = label_raw.split(b"\0", 1)[0].decode()
        return SyncMessage(label=label, frame_count=frame_count)


# --------------------
# Shared-memory metadata
# --------------------


@dataclass
class FrameInfo:
    width: int
    """
    `uint16_t`
    """
    height: int
    """
    `uint16_t`
    """
    channels: int
    """
    `uint8_t`
    """
    depth: Depth
    """
    `uint8_t`

    OpenCV pixel depth definition
    (CV_8U, CV_16U, CV_16S, CV_32S, CV_32F, CV_64F)
    """
    buffer_size: int
    """
    `uint32_t`
    """
    pixel_format: PixelFormat

    # width, height, channels, depth, buffer_size, pixel_format
    PACK_FMT = "=HHBBIB"

    @staticmethod
    def size() -> int:
        return struct.calcsize(FrameInfo.PACK_FMT)

    @staticmethod
    def unmarshal(data: bytes) -> "FrameInfo":
        (
            width,
            height,
            channels,
            depth_raw,
            buffer_size,
            pixel_format_raw,
        ) = struct.unpack(FrameInfo.PACK_FMT, data[: FrameInfo.size()])
        return FrameInfo(
            width=width,
            height=height,
            channels=channels,
            depth=Depth(depth_raw),
            buffer_size=buffer_size,
            pixel_format=PixelFormat(pixel_format_raw),
        )


@dataclass
class FrameMetadata:
    # frame_count + FrameInfo (strip the first endianness indicator)
    PACK_FMT = "=I" + FrameInfo.PACK_FMT[1:]

    # properties
    frame_count: int
    info: FrameInfo

    @staticmethod
    def size() -> int:
        return struct.calcsize(FrameMetadata.PACK_FMT)

    @staticmethod
    def unmarshal(data: bytes) -> "FrameMetadata":
        (
            frame_count,
            width,
            height,
            channels,
            depth_raw,
            buffer_size,
            pixel_format_raw,
        ) = struct.unpack(FrameMetadata.PACK_FMT, data[: FrameMetadata.size()])
        return FrameMetadata(
            frame_count=frame_count,
            info=FrameInfo(
                width=width,
                height=height,
                channels=channels,
                depth=Depth(depth_raw),
                buffer_size=buffer_size,
                pixel_format=PixelFormat(pixel_format_raw),
            ),
        )
