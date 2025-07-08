from logging import getLogger
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

from .msg import SyncMessage, FRAME_TOPIC_MAGIC
from .shm import SharedMemory

NDArray = np.ndarray


class CvMmapClient:
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
        *,
        shm_name: Optional[str] = None,
        zmq_addr: Optional[str] = None,
    ):
        """Create a CvMmapClient.

        Parameters
        ----------
        name
            Base name of the video source (e.g. "default"). The shared-memory
            segment is assumed to be ``cvmmap_{name}`` and the ZMQ publisher
            address ``ipc:///tmp/{shm_name}``.
        shm_name, zmq_addr
            Override the derived shared-memory name or ZMQ address if you need
            a non-standard layout.
        """

        self._name = name
        # Derive resource names following the C++ producer convention.
        self._shm_name = shm_name or f"cvmmap_{name}"
        self._zmq_addr = zmq_addr or f"ipc:///tmp/{self._shm_name}"

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

    def _init_shm(self, size: int):
        """
        Internal use only.

        Initialize shared memory buffer.
        """
        # disable tracking
        if self._shm is None:
            self._shm = SharedMemory(  # pylint: disable=unexpected-keyword-arg
                name=self._shm_name, create=False, size=size, track=False
            )
        else:
            raise ValueError("Shared memory already initialized")

    async def __aiter__(self) -> AsyncGenerator[tuple[NDArray, SyncMessage], None]:
        """
        Asynchronous generator that yields numpy array of image.
        """
        while True:
            events = await self._poller.poll()
            for socket, event in events:
                if event & zmq.POLLIN:
                    message = await socket.recv()
                    message = cast(bytes, message)
                    # it's the frame topic, ignore it
                    try:
                        sync_message = SyncMessage.unmarshal(message)
                        if self._image_buffer is None:
                            self._init_shm(sync_message.buffer_size)
                            assert self._shm is not None
                            self._image_buffer = np.ndarray(
                                (
                                    sync_message.height,
                                    sync_message.width,
                                    sync_message.channels,
                                ),
                                dtype=np.uint8,
                                buffer=self._shm.buf,
                            )
                        yield self._image_buffer, sync_message
                    except StructError as e:
                        getLogger(__name__).exception(e)
                        continue


class CvMmapConfig(TypedDict):
    name: str
    # Optional overrides for non-standard setups
    shm_name: Optional[str]
    zmq_addr: Optional[str]
