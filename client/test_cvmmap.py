from cvmmap import CvMmapClient
from loguru import logger
import click
import anyio
import cv2

# note that no beginning slash is needed
NAME = "camera_5602"

async def main():
    client = CvMmapClient(NAME)
    logger.info("created")
    async for im, meta in client:
        cv2.imshow("image", im)
        cv2.waitKey(1)


def run_main():
    anyio.run(main)


if __name__ == "__main__":
    run_main()
