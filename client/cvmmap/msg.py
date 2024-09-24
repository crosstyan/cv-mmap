from dataclasses import dataclass
import struct


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
    depth: int
    """
    `uint8_t`

    OpenCV pixel depth definition
    (CV_8U, CV_16U, CV_16S, CV_32S, CV_32F, CV_64F)
    """
    buffer_size: int
    """
    `uint32_t`
    """

    @staticmethod
    def unmarshal(data: bytes) -> "SyncMessage":
        frame_count, width, height, channels, depth, buffer_size = struct.unpack(
            "=IHHBBI", data
        )
        return SyncMessage(
            frame_count=frame_count,
            width=width,
            height=height,
            channels=channels,
            depth=depth,
            buffer_size=buffer_size,
        )
