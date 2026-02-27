from cvmmap import CvMmapClient
from loguru import logger
import click
import anyio
import cv2

# note that no beginning slash is needed

async def main(name:str):
    client = CvMmapClient(name)
    logger.info("created")
    async for im, meta in client:
        cv2.imshow("image", im)
        cv2.waitKey(1)


@click.command()
@click.argument("name", required=True, type=str)
def run_main(name: str):
    anyio.run(main, name)


if __name__ == "__main__":
    run_main()
