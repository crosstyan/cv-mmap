from cvmmap import CvMmapClient
import anyio
from loguru import logger
import cv2

# note that no beginning slash is needed
NAME = "default"


async def main():
    client = CvMmapClient(NAME)
    async for im, msg in client:
        cv2.imshow("image", im)
        cv2.waitKey(1)


def run_main():
    anyio.run(main)


if __name__ == "__main__":
    run_main()
