from pathlib import Path
from struct import error as StructError
from typing import AsyncContextManager, AsyncGenerator, Generator, Optional, cast

import numpy as np
import zmq
from logging import getLogger
from zmq import Socket
from zmq.asyncio import Context, Poller

from .msg import SyncMessage
from .shm import SharedMemory

NDArray = np.ndarray


class CvMmapClient:
    _shm_name: str
    _zmq_addr: str

    _ctx: Context
    _sock: Socket
    _poller: Poller

    _image_buffer: Optional[NDArray] = None
    _shm: Optional[SharedMemory] = None

    def __init__(self, shm_name: str, zmq_addr: str):
        self._shm_name = shm_name
        self._zmq_addr = zmq_addr

        self._ctx = Context.instance()
        self._sock = self._ctx.socket(zmq.PULL)
        self._sock.connect(self._zmq_addr)
        self._poller = Poller()
        self._poller.register(self._sock, zmq.POLLIN)

        self._image_buffer = None
        self._shm = None

    def _init_shm(self, size: int):
        """
        Interal use only.

        Initialize shared memory buffer.
        """
        # disable tracking
        if self._shm is None:
            self._shm = SharedMemory(  # pylint: disable=unexpected-keyword-arg
                name=self._shm_name, create=False, size=size, track=False
            )
        else:
            raise ValueError("Shared memory already initialized")

    async def __aiter__(self) -> AsyncGenerator[NDArray, None]:
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
                        yield self._image_buffer
                    except StructError as e:
                        getLogger(__name__).exception(e)
                        continue
