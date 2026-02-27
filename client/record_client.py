import asyncio
import click
from pathlib import Path
from datetime import datetime
from loguru import logger
import cv2

from cvmmap import CvMmapClient


from typing import Optional

# We use asyncio directly since cvmmap is built on zmq.asyncio.
# The core loop must run asynchronously to process frames as they arrive.
async def record_client(stream_name: str, out_path: Path, force_fps: Optional[float] = None):
    out_path.mkdir(parents=True, exist_ok=True)

    logger.info("Controls:")
    logger.info("  [SPACE] - Start/Stop recording")
    logger.info("  [q] or [ESC] - Quit")

    client = CvMmapClient(stream_name)

    # State variables for recording
    recording = False
    writer = None
    frame_count = 0

    # FPS estimation using timestamps from metadata
    last_timestamps: list[int] = []
    MAX_FPS_SAMPLES = 60
    try:
        # 2. Async iteration over the frames
        # The __aiter__ handles ZeroMQ conflation and memory mapping behind the scenes.
        # It yields a numpy array (image) and the frame metadata.
        async for frame, meta in client:
            # Update FPS estimation queue using metadata timestamps (nanoseconds)
            last_timestamps.append(meta.timestamp_ns)
            if len(last_timestamps) > MAX_FPS_SAMPLES:
                last_timestamps.pop(0)
            # --- PREVIEW ---
            # Create a copy if you plan to modify it for display, otherwise
            # just show the shared memory view directly to save CPU.
            display_frame = frame.copy() if recording else frame

            # Add visual indicator if recording
            if recording:
                _ = cv2.circle(display_frame, (30, 30), 10, (0, 0, 255), -1)
                _ = cv2.putText(
                    display_frame,
                    f"REC {frame_count}",
                    (50, 35),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.7,
                    (0, 0, 255),
                    2,
                )
            cv2.namedWindow(f"Preview: {stream_name}", cv2.WINDOW_NORMAL)
            cv2.imshow(f"Preview: {stream_name}", display_frame)

            # --- RECORDING ---
            if recording and writer is not None:
                # writer.write blocks slightly, but NVENC/hardware encoders are fast.
                # If disk I/O blocks too much, this could cause ZeroMQ conflate to
                # drop frames (which is intended behavior for real-time systems to avoid memory explosion).
                writer.write(frame)
                frame_count += 1

            # --- INPUT HANDLING ---
            # cv2.waitKey(1) allows OpenCV GUI events to process
            key = cv2.waitKey(1) & 0xFF

            if key in (ord("q"), 27):  # q or ESC
                break

            elif key == ord(" "):  # Spacebar toggles recording
                recording = not recording

                if recording:
                    # START RECORDING
                    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                    filename = out_path / f"record_{stream_name}_{timestamp}.mp4"

                    height: int = int(frame.shape[0])
                    width: int = int(frame.shape[1])

                    # Calculate FPS from rolling average if enough samples, else default to 30.0
                    fps = 30.0
                    if force_fps is not None:
                        fps = force_fps
                        logger.info(f"Using forced FPS from CLI: {fps}")
                    elif len(last_timestamps) >= 2:
                        duration_ns = last_timestamps[-1] - last_timestamps[0]
                        if duration_ns > 0:
                            fps_estimated = (len(last_timestamps) - 1) / (duration_ns / 1e9)
                            fps = round(fps_estimated, 2)
                            logger.info(f"Estimated FPS based on {len(last_timestamps)} samples: {fps}")
                        else:
                            logger.warning("Duration is 0, using default 30.0 FPS")
                    else:
                        logger.warning("Not enough samples to estimate FPS, using default 30.0")
                    # --- CHOOSING THE CODEC ---
                    # FOURCC (Four-Character Code) specifies the codec.
                    #
                    # For hardware acceleration (NVIDIA GPU):
                    # Use 'h264_nvenc' or 'hevc_nvenc'.
                    # Note: OpenCV must be compiled with FFmpeg + NVENC support to use hardware encoders directly via VideoWriter.
                    # Usually, the string 'avc1' or 'mp4v' combined with the right backend (CAP_FFMPEG) is used.
                    #
                    # Common fallbacks:
                    # 'mp4v' - standard MPEG-4 (software, widely supported)
                    # 'avc1' - H.264 (usually software libx264, highly compatible)
                    # 'hevc' - H.265 (better compression, heavier CPU)

                    # Using 'mp4v' as a safe cross-platform default for this demo.
                    fourcc = cv2.VideoWriter.fourcc(*"mp4v")

                    # Initialize the writer
                    writer = cv2.VideoWriter(
                        str(filename), fourcc, fps, (width, height)
                    )

                    if not writer.isOpened():
                        logger.error(f"Failed to open video writer for {filename}!")
                        recording = False
                    else:
                        frame_count = 0
                        logger.success(
                            f"Started recording to {filename} ({width}x{height})"
                        )

                else:
                    # STOP RECORDING
                    if writer:
                        writer.release()
                        writer = None
                    logger.info(f"Stopped recording. Saved {frame_count} frames.")

    except asyncio.CancelledError:
        logger.info("Client task cancelled.")
    finally:
        # Cleanup
        if writer:
            writer.release()
        cv2.destroyAllWindows()
        logger.info("Exiting.")


@click.command()
@click.argument("stream_name")
@click.option(
    "--out",
    "-o",
    type=click.Path(path_type=Path),
    default=Path("./recordings"),
    help="Output directory",
)
@click.option(
    "--fps",
    type=float,
    default=None,
    help="Force a specific FPS (otherwise estimated from timestamps)",
)
def cli(stream_name: str, out: Path, fps: Optional[float]):
    """cv-mmap recording client"""
    try:
        # Run the async loop
        asyncio.run(record_client(stream_name, out, fps))
    except KeyboardInterrupt:
        logger.info("Interrupted by user.")


if __name__ == "__main__":
    cli()
