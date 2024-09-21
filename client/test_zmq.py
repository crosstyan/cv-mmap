import zmq
import anyio
from loguru import logger
from zmq.asyncio import Context, Poller
from typing import cast, Final
from sys import byteorder

URL = "ipc:///tmp/0"

global_ctx = Context.instance()


# https://github.com/zeromq/pyzmq/blob/main/examples/poll/pubsub.py
# https://github.com/zeromq/pyzmq/blob/main/examples/asyncio/coroutines.py
async def async_main():
    logger.info("Starting client")
    socket = global_ctx.socket(zmq.PULL)
    socket.connect(URL)
    logger.info("Connected to {}", URL)
    poller = Poller()
    poller.register(socket, zmq.POLLIN)
    while True:
        events = await poller.poll()
        for socket, event in events:
            if event & zmq.POLLIN:
                message = await socket.recv()
                message = cast(bytes, message)
                num = int.from_bytes(message, byteorder)
                logger.info("Received {}", num)


def main():
    anyio.run(async_main)


if __name__ == "__main__":
    main()
