# OpenCV IPC capture

Capture video frames from a camera using OpenCV and share them with other processes using shared memory.
The stream could be a GStreamer pipeline or a camera device, depending on the platform/API.
Use [ZeroMQ](https://zeromq.org/) to notify other processes when a new frame is available. (for synchronization)
The consumer process SHOULD NOT write to the shared memory, only read/clone the data.

- [ajaygunalan/IPC_SHM](https://github.com/ajaygunalan/IPC_SHM)
- [khomin/electron_camera_ffmpeg](https://github.com/khomin/electron_camera_ffmpeg)
- [khomin/electron_ffmpeg_addon_camera](https://github.com/khomin/electron_ffmpeg_addon_camera)
- [OpenIPC/wiki](https://github.com/OpenIPC/wiki/blob/master/en/faq.md)
