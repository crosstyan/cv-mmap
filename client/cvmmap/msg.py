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
        # C++ format: magic(1) + frame_count(4) + label_len(1) + label_data(variable)
        magic = int(struct.unpack("=B", data[0:1])[0])
        assert magic == FRAME_TOPIC_MAGIC, "Invalid topic magic"

        frame_count = int(struct.unpack("=I", data[1:5])[0])
        label_len = int(struct.unpack("=B", data[5:6])[0])
        label_data = data[6 : 6 + label_len]
        label = label_data.decode()

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
