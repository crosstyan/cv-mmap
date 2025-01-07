from dataclasses import dataclass
import struct
from enum import Enum, auto


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
    frame_count: int
    """
    `uint32_t`
    """
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

    @staticmethod
    def unmarshal(data: bytes) -> "SyncMessage":
        (
            frame_count,
            width,
            height,
            channels,
            depth_raw,
            buffer_size,
            pixel_format_raw,
        ) = struct.unpack("=IHHBBIB", data)
        depth = Depth(depth_raw)
        pixel_format = PixelFormat(pixel_format_raw)
        return SyncMessage(
            frame_count=frame_count,
            width=width,
            height=height,
            channels=channels,
            depth=depth,
            buffer_size=buffer_size,
            pixel_format=pixel_format,
        )
