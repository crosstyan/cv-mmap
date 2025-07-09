from logging import getLogger
import struct
from struct import error as StructError
from typing import (
    AsyncGenerator,
    Optional,
    cast,
    TypedDict,
)

import numpy as np
import zmq
from zmq import Socket
from zmq.asyncio import Context, Poller

from .msg import SyncMessage, FrameMetadata, FRAME_TOPIC_MAGIC
from .shm import SharedMemory

NDArray = np.ndarray


class CvMmapClient:
    """
    A client for the CvMmap protocol
    """

    _name: str
    _shm_name: str
    _zmq_addr: str

    _ctx: Context
    _sock: Socket
    _poller: Poller

    _image_buffer: Optional[NDArray] = None
    _shm: Optional[SharedMemory] = None

    def _subscribe(self):
        """
        manually trigger the subscription to the topic.

        https://github.com/zeromq/libzmq/issues/1688
        https://stackoverflow.com/questions/57901180/only-keep-latest-multipart-message-in-subscriber-with-pyzmq-pub-sub-socket
        """
        self._sock.subscribe(bytes([FRAME_TOPIC_MAGIC]))

    def _unsubscribe(self):
        """
        manually trigger the un-subscription to the topic.
        """
        self._sock.unsubscribe(bytes([FRAME_TOPIC_MAGIC]))

    def __init__(
        self,
        name: str,
    ):
        """Create a CvMmapClient.

        Parameters
        ----------
        name
            Base name of the video source (e.g. "default"). The shared-memory
            segment is assumed to be ``cvmmap_{name}`` and the ZMQ publisher
            address ``ipc:///tmp/{shm_name}`` by convention.
        """

        self._name = name
        # convention over configuration
        self._shm_name = f"cvmmap_{name}"
        self._zmq_addr = f"ipc:///tmp/{self._shm_name}"

        self._ctx = Context.instance()
        self._sock = self._ctx.socket(zmq.SUB)
        # In Python, you set the CONFLATE option before you connect to the socket
        self._sock.setsockopt(zmq.CONFLATE, 1)
        self._sock.connect(self._zmq_addr)
        self._subscribe()
        self._poller = Poller()
        self._poller.register(self._sock, zmq.POLLIN)

        self._image_buffer = None
        self._shm = None

    _SHM_PAYLOAD_OFFSET = 256

    def _read_metadata(self) -> FrameMetadata:
        """Read and decode the `FrameMetadata` structure from shared memory.

        The memory layout written by the C++ producer is:

        ```
        0-7   : "CV-MMAP\0" magic bytes
        8-…  : FrameMetadata packed struct (frame_count + FrameInfo)
        ```

        This function validates the magic prefix and then uses the Python
        struct definitions to unpack the metadata that follows.
        """
        assert self._shm is not None, "Shared memory not attached"

        # The shared-memory metadata starts with the 8-byte magic string
        # "CV-MMAP\0" followed by the packed FrameMetadata bytes.
        from .msg import CV_MMAP_MAGIC, CV_MMAP_MAGIC_LEN

        # Validate magic
        magic = bytes(self._shm.buf[:CV_MMAP_MAGIC_LEN])
        if magic != CV_MMAP_MAGIC:
            raise RuntimeError(
                f"Invalid CV_MMAP magic prefix in shared memory: {magic!r} (expected {CV_MMAP_MAGIC!r})"
            )

        start = CV_MMAP_MAGIC_LEN
        end = start + FrameMetadata.size()
        return FrameMetadata.unmarshal(bytes(self._shm.buf[start:end]))
    
    def _read_metadata_unchecked(self) -> FrameMetadata:
        """
        Read and decode the `FrameMetadata` structure from shared memory directly without checking the magic
        """
        assert self._shm is not None, "Shared memory not attached"
        from .msg import CV_MMAP_MAGIC_LEN
        start = CV_MMAP_MAGIC_LEN
        end = start + FrameMetadata.size()
        return FrameMetadata.unmarshal(bytes(self._shm.buf[start:end]))

    def _ensure_memory(self):
        """Attach to shared memory and initialize the numpy view if necessary."""
        if self._shm is not None and self._image_buffer is not None:
            return

        if self._shm is None:
            self._shm = SharedMemory(  # pylint: disable=unexpected-keyword-arg
                name=self._shm_name, create=False, track=False
            )

        # Read metadata once and build numpy view if not yet created (this also validates magic)
        meta = self._read_metadata()
        if self._image_buffer is None:
            start = self._SHM_PAYLOAD_OFFSET
            end = start + meta.info.buffer_size
            mv = self._shm.buf[start:end]
            self._image_buffer = np.ndarray(
                (meta.info.height, meta.info.width, meta.info.channels),
                dtype=np.uint8,
                buffer=mv,
            )

    async def __aiter__(self) -> AsyncGenerator[tuple[NDArray, FrameMetadata], None]:
        """
        Asynchronous generator that yields numpy array of image.
        """
        while True:
            events = await self._poller.poll()
            for socket, event in events:
                if event & zmq.POLLIN:
                    message = await socket.recv()
                    message = cast(bytes, message)

                    try:
                        sync_message = SyncMessage.unmarshal(message)
                        self._ensure_memory()
                        assert self._image_buffer is not None

                        if sync_message.label != self._name:
                            raise RuntimeError(
                                f"Label mismatch: expected '{self._name}', got '{sync_message.label}'"
                            )

                        metadata = self._read_metadata_unchecked()
                        yield self._image_buffer, metadata
                    except StructError as e:
                        getLogger(__name__).exception(e)
                        continue


class CvMmapConfig(TypedDict):
    name: str
    # Optional overrides for non-standard setups
    shm_name: Optional[str]
    zmq_addr: Optional[str]
