from cvmmap import CvMmapClient
import anyio
from loguru import logger
import cv2

# note that no beginning slash is needed
SHM_NAME = "psm_default"
ZMQ_ADDR = "ipc:///tmp/0"


async def main():
    client = CvMmapClient(SHM_NAME, ZMQ_ADDR)
    async for im in client.polling():
        cv2.imshow("image", im)
        cv2.waitKey(1)


def run_main():
    anyio.run(main)


if __name__ == "__main__":
    run_main()
