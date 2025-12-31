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

from .msg import (
    SyncMessage,
    FrameMetadata,
    FrameInfo,
    ModuleStatusMessage,
    ControlMessageRequest,
    ControlMessageResponse,
    FRAME_TOPIC_MAGIC,
    MODULE_STATUS_MAGIC,
    CV_MMAP_MAGIC,
    CV_MMAP_MAGIC_LEN,
    CONTROL_MSG_CMD_GENERIC,
    CONTROL_MSG_CMD_RESET_FRAME_COUNT,
    CONTROL_RESPONSE_OK,
    MODULE_STATUS_OFFLINE,
    MODULE_STATUS_STREAM_RESET,
)
from .shm import SharedMemory

NDArray = np.ndarray

# Re-export message types for convenience
__all__ = [
    "CvMmapClient",
    "CvMmapRequestClient",
    "CvMmapConfig",
    "SyncMessage",
    "FrameMetadata",
    "FrameInfo",
    "ModuleStatusMessage",
    "ControlMessageRequest",
    "ControlMessageResponse",
]


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
        self._sock.subscribe(bytes([MODULE_STATUS_MAGIC]))

    def _unsubscribe(self):
        """
        manually trigger the un-subscription to the topic.
        """
        self._sock.unsubscribe(bytes([FRAME_TOPIC_MAGIC]))
        self._sock.unsubscribe(bytes([MODULE_STATUS_MAGIC]))

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
        8-…  : FrameMetadata packed struct (versions + frame_count + timestamp_ns + FrameInfo)
        ```

        This function validates the magic prefix and then uses the Python
        struct definitions to unpack the metadata that follows.
        """
        assert self._shm is not None, "Shared memory not attached"

        # Validate magic
        magic = bytes(self._shm.buf[:CV_MMAP_MAGIC_LEN])
        if magic != CV_MMAP_MAGIC:
            raise RuntimeError(
                f"Invalid CV_MMAP magic prefix in shared memory: {magic!r} (expected {CV_MMAP_MAGIC!r})"
            )

        start = CV_MMAP_MAGIC_LEN
        end = start + FrameMetadata.size() - CV_MMAP_MAGIC_LEN
        return FrameMetadata.unmarshal(bytes(self._shm.buf[start:end]))

    def _read_metadata_unchecked(self) -> FrameMetadata:
        """
        Read and decode the `FrameMetadata` structure from shared memory directly without checking the magic
        """
        assert self._shm is not None, "Shared memory not attached"
        start = CV_MMAP_MAGIC_LEN
        end = start + FrameMetadata.size() - CV_MMAP_MAGIC_LEN
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

        Raises
        ------
        StopAsyncIteration
            When module goes offline or stream is reset.
        RuntimeError
            When label mismatch or other errors occur.
        """
        while True:
            events = await self._poller.poll()
            for socket, event in events:
                if event & zmq.POLLIN:
                    message = await socket.recv()
                    message = cast(bytes, message)

                    # Check the magic byte to determine message type
                    if len(message) < 1:
                        raise RuntimeError("Received empty message")

                    magic = message[0]

                    if magic == MODULE_STATUS_MAGIC:
                        # Handle module status message
                        status_msg = ModuleStatusMessage.unmarshal(message)
                        if status_msg.module_status in (
                            MODULE_STATUS_OFFLINE,
                            MODULE_STATUS_STREAM_RESET,
                        ):
                            status_name = (
                                "OFFLINE"
                                if status_msg.module_status == MODULE_STATUS_OFFLINE
                                else "STREAM_RESET"
                            )
                            getLogger(__name__).info(
                                "Module '%s' status: %s, stopping generator",
                                status_msg.label,
                                status_name,
                            )
                            return
                        continue

                    if magic == FRAME_TOPIC_MAGIC:
                        # Handle sync message (frame notification)
                        sync_message = SyncMessage.unmarshal(message)
                        self._ensure_memory()
                        assert self._image_buffer is not None

                        if sync_message.label != self._name:
                            raise RuntimeError(
                                f"Label mismatch: expected '{self._name}', got '{sync_message.label}'"
                            )

                        metadata = self._read_metadata_unchecked()
                        yield self._image_buffer, metadata
                    else:
                        raise RuntimeError(f"Unknown message magic: {magic:#x}")


class CvMmapConfig(TypedDict):
    name: str
    # Optional overrides for non-standard setups
    shm_name: Optional[str]
    zmq_addr: Optional[str]


class CvMmapRequestClient:
    """
    A client for sending control requests to the CvMmap server.

    Uses ZMQ REQ/REP pattern to send control messages and receive responses.
    """

    _name: str
    _shm_name: str
    _zmq_addr: str

    _ctx: Context
    _sock: Socket

    def __init__(
        self,
        name: str,
        zmq_addr: Optional[str] = None,
    ):
        """Create a CvMmapRequestClient.

        Parameters
        ----------
        name
            Base name of the video source (e.g. "default"). Used as the label
            in control messages.
        zmq_addr
            Optional ZMQ REQ socket address. If not provided, defaults to
            ``ipc:///tmp/cvmmap_{name}_req`` by convention.
        """
        self._name = name
        self._shm_name = f"cvmmap_{name}"
        self._zmq_addr = zmq_addr or f"ipc:///tmp/{self._shm_name}_req"

        self._ctx = Context.instance()
        self._sock = self._ctx.socket(zmq.REQ)
        self._sock.connect(self._zmq_addr)

    async def send_request(
        self,
        command_id: int,
        request_message: bytes = b"",
        timeout_ms: int = 5000,
    ) -> ControlMessageResponse:
        """
        Send a control request and wait for response.

        Parameters
        ----------
        command_id
            The command ID to send.
        request_message
            Optional additional data to include in the request.
        timeout_ms
            Timeout in milliseconds to wait for response.

        Returns
        -------
        ControlMessageResponse
            The response from the server.

        Raises
        ------
        TimeoutError
            If no response is received within the timeout.
        """
        request = ControlMessageRequest(
            label=self._name,
            command_id=command_id,
            request_message=request_message,
        )

        await self._sock.send(request.marshal())

        # Poll with timeout
        poller = Poller()
        poller.register(self._sock, zmq.POLLIN)
        events = await poller.poll(timeout=timeout_ms)

        if not events:
            raise TimeoutError(f"No response received within {timeout_ms}ms")

        response_data = await self._sock.recv()
        return ControlMessageResponse.unmarshal(cast(bytes, response_data))

    async def reset_frame_count(self, timeout_ms: int = 5000) -> ControlMessageResponse:
        """
        Send a reset frame count command.

        Parameters
        ----------
        timeout_ms
            Timeout in milliseconds to wait for response.

        Returns
        -------
        ControlMessageResponse
            The response from the server.
        """
        return await self.send_request(
            command_id=CONTROL_MSG_CMD_RESET_FRAME_COUNT,
            timeout_ms=timeout_ms,
        )

    def close(self):
        """Close the ZMQ socket."""
        if self._sock is not None:
            self._sock.close()

    def __del__(self):
        self.close()
