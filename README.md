# OpenCV IPC capture

Capture video frames from a camera using OpenCV and share them with other processes using shared memory.
The stream could be a GStreamer pipeline or a camera device, depending on the platform/API.
Use [ZeroMQ](https://zeromq.org/) to notify other processes when a new frame is available. (for synchronization)
The consumer process SHOULD NOT write to the shared memory, only read/clone the data.

- [ajaygunalan/IPC_SHM](https://github.com/ajaygunalan/IPC_SHM)
- [khomin/electron_camera_ffmpeg](https://github.com/khomin/electron_camera_ffmpeg)
- [khomin/electron_ffmpeg_addon_camera](https://github.com/khomin/electron_ffmpeg_addon_camera)
- [OpenIPC/wiki](https://github.com/OpenIPC/wiki/blob/master/en/faq.md)

```bash
# opencv/build
cmake .. -DOPENCV_EXTRA_MODULES_PATH=/Volumes/External/Code/opencv_contrib/modules/ \
    -DCMAKE_CXX_STANDARD=17 \
    -DBUILD_JASPER=OFF \
    -DBUILD_JPEG=OFF \
    -DBUILD_OPENEXR=OFF \
    -DBUILD_OPENJPEG=OFF \
    -DBUILD_PERF_TESTS=OFF \
    -DBUILD_PNG=OFF \
    -DBUILD_PROTOBUF=OFF \
    -DBUILD_TBB=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_TIFF=OFF \
    -DBUILD_WEBP=OFF \
    -DBUILD_ZLIB=OFF \
    -DBUILD_opencv_hdf=OFF \
    -DBUILD_opencv_java=OFF \
    -DBUILD_opencv_text=ON \
    -DOPENCV_ENABLE_NONFREE=ON \
    -DOPENCV_GENERATE_PKGCONFIG=ON \
    -DPROTOBUF_UPDATE_FILES=ON \
    -DWITH_1394=OFF \
    -DWITH_CUDA=OFF \
    -DWITH_EIGEN=ON \
    -DWITH_FFMPEG=ON \
    -DWITH_GPHOTO2=OFF \
    -DWITH_GSTREAMER=ON \
    -DWITH_JASPER=OFF \
    -DWITH_OPENEXR=ON \
    -DWITH_OPENGL=OFF \
    -DWITH_OPENVINO=ON \
    -DWITH_QT=OFF \
    -DWITH_TBB=ON \
    -DWITH_VTK=ON \
    -DBUILD_opencv_python2=OFF \
    -DBUILD_opencv_python3=ON
```

```bash
HOMEBREW_DEVELOPER=1 brew install --build-from-source -v --formula ./opencv.rb
```

## Dependencies

### Arch Linux

```bash
sudo pacman -S opencv \
    gst-plugins-base  \
    gst-plugins-good  \
    gst-plugins-bad  \
    gst-plugins-ugly  \
    gstreamer  \
    cppzmq  \
    spdlog \
    vtk \
    glew \
    hdf5
```
