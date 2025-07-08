import struct
from dataclasses import dataclass
from enum import Enum, auto

FRAME_TOPIC_MAGIC = 0x7D


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
    label: str
    frame_count: int

    @staticmethod
    def unmarshal(data: bytes) -> "SyncMessage":
        LABEL_LEN = 24
        fmt = f"=B{LABEL_LEN}sI"
        magic, label_raw, frame_count = struct.unpack(fmt, data[: struct.calcsize(fmt)])
        assert magic == FRAME_TOPIC_MAGIC, "Invalid topic magic"
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

    PACK_FMT = "=HHBBIB"  # width, height, channels, depth, buffer_size, pixel_format

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
    PACK_FMT = "=I" + FrameInfo.PACK_FMT

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
