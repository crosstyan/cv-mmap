import zmq
import anyio
from loguru import logger
from zmq.asyncio import Context, Poller
from typing import cast, Optional
from sys import byteorder
import numpy as np
import struct
from dataclasses import dataclass
import sys
import threading
import cv2
from pathlib import Path
from multiprocessing import resource_tracker as _mprt
from multiprocessing import shared_memory as _mpshm

if sys.version_info >= (3, 13):
    SharedMemory = _mpshm.SharedMemory
else:

    class SharedMemory(_mpshm.SharedMemory):
        """
        copy from https://github.com/python/cpython/issues/82300#issuecomment-2169035092
        """

        __lock = threading.Lock()

        def __init__(
            self,
            name: str | None = None,
            create: bool = False,
            size: int = 0,
            *,
            track: bool = True,
        ) -> None:
            self._track = track

            # if tracking, normal init will suffice
            if track:
                super().__init__(name=name, create=create, size=size)
                return

            # lock so that other threads don't attempt to use the
            # register function during this time
            with self.__lock:
                # temporarily disable registration during initialization
                orig_register = _mprt.register
                _mprt.register = self.__tmp_register

                # initialize; ensure original register function is
                # re-instated
                try:
                    super().__init__(name=name, create=create, size=size)
                finally:
                    _mprt.register = orig_register

        @staticmethod
        def __tmp_register(*args, **kwargs) -> None:
            return

        def unlink(self) -> None:
            if _mpshm._USE_POSIX and self._name:  # pylint: disable=protected-access
                _mpshm._posixshmem.shm_unlink(
                    self._name
                )  # pylint: disable=protected-access
                if self._track:
                    _mprt.unregister(self._name, "shared_memory")


@dataclass
class SyncMessage:
    # uint32_t
    frame_count: int
    # uint16_t
    width: int
    # uint16_t
    height: int
    # uint8_t
    channels: int
    # uint8_t
    depth: int
    # uint32_t
    buffer_size: int

    @staticmethod
    def unmarshal(data: bytes) -> "SyncMessage":
        frame_count, width, height, channels,depth, buffer_size = struct.unpack("=IHHBBI", data)
        return SyncMessage(
            frame_count=frame_count,
            width=width,
            height=height,
            channels=channels,
            depth=depth,
            buffer_size=buffer_size,
        )


URL = "ipc:///tmp/0"
# note that python would add `/psm_` prefix to the name
SHM_NAME = "psm_default"
OUTPUT_DIR = Path("output")

# https://docs.python.org/3/library/multiprocessing.shared_memory.html


# https://github.com/zeromq/pyzmq/blob/main/examples/poll/pubsub.py
# https://github.com/zeromq/pyzmq/blob/main/examples/asyncio/coroutines.py
# https://docs.python.org/3/library/multiprocessing.shared_memory.html
async def async_main():
    logger.info("Starting client")
    ctx = Context.instance()
    socket = ctx.socket(zmq.PULL)
    socket.connect(URL)
    logger.info("Connected to {}", URL)
    poller = Poller()
    poller.register(socket, zmq.POLLIN)

    shm: Optional[SharedMemory] = None
    image_buffer: Optional[np.ndarray] = None

    def init_shm(size: int):
        nonlocal shm
        if shm is None:
            # disable tracking to avoid unlinking the shared memory on exit
            shm = SharedMemory(  # pylint: disable=unexpected-keyword-arg
                name=SHM_NAME, create=False, size=size, track=False
            )

    while True:
        events = await poller.poll()
        # https://stackoverflow.com/questions/77285558/why-does-python-shared-memory-implicitly-unlinked-on-exit
        # https://github.com/python/cpython/issues/82300
        for socket, event in events:
            if event & zmq.POLLIN:
                message = await socket.recv()
                message = cast(bytes, message)

                def to_hexstr(data: bytes):
                    return " ".join(f"{b:02x}" for b in data) + f" ({len(data)})"

                try:
                    sync_message = SyncMessage.unmarshal(message)
                    logger.info("Received sync message: {}", sync_message)
                    if image_buffer is None:
                        init_shm(sync_message.buffer_size)
                        image_buffer = np.ndarray(
                            (
                                sync_message.height,
                                sync_message.width,
                                sync_message.channels,
                            ),
                            dtype=np.uint8,
                            buffer=shm.buf,
                        )
                    im = image_buffer.copy()
                    cv2.imwrite(
                        str(OUTPUT_DIR / "{}.png".format(sync_message.frame_count)),
                        im,
                    )
                except struct.error as e:
                    logger.exception(e)
                    logger.info("Received {}", to_hexstr(message))


def main():
    anyio.run(async_main)


if __name__ == "__main__":
    main()
