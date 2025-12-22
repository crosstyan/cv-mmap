#pragma once
#include <cstdint>
#include <string_view>

namespace app {
/// @note use with `pixel_format` field in `frame_info_t`
enum class PixelFormat : uint8_t {
	/// usually 24bit RGB (8bit per channel, depth=U8)
	RGB = 0,
	BGR,
	RGBA,
	BGRA,
	/// channel=1
	GRAY,
	YUV,
	YUYV,
};

/// @note use with `depth` field in `frame_info_t`
enum class Depth : uint8_t {
	U8  = 0,
	S8  = 1,
	U16 = 2,
	S16 = 3,
	S32 = 4,
	F32 = 5,
	F64 = 6,
	F16 = 7,
};


/** @brief cv::VideoCapture API backends identifier.

Select preferred API for a capture object.
To be used in the VideoCapture::VideoCapture() constructor or VideoCapture::open()

@note
-   Backends are available only if they have been built with your OpenCV binaries.
See @ref videoio_overview for more information.
-   Microsoft Media Foundation backend tries to use hardware accelerated transformations
if possible. Environment flag "OPENCV_VIDEOIO_MSMF_ENABLE_HW_TRANSFORMS" set to 0
disables it and may improve initialization time. More details:
https://learn.microsoft.com/en-us/windows/win32/medfound/mf-readwrite-enable-hardware-transforms

@note this enum is copy directly from `opencv2/videoio.hpp`
*/
enum VideoCaptureAPIs {
	CAP_ANY           = 0,            //!< Auto detect == 0
	CAP_VFW           = 200,          //!< Video For Windows (obsolete, removed)
	CAP_V4L           = 200,          //!< V4L/V4L2 capturing support
	CAP_V4L2          = CAP_V4L,      //!< Same as CAP_V4L
	CAP_FIREWIRE      = 300,          //!< IEEE 1394 drivers
	CAP_FIREWARE      = CAP_FIREWIRE, //!< Same value as CAP_FIREWIRE
	CAP_IEEE1394      = CAP_FIREWIRE, //!< Same value as CAP_FIREWIRE
	CAP_DC1394        = CAP_FIREWIRE, //!< Same value as CAP_FIREWIRE
	CAP_CMU1394       = CAP_FIREWIRE, //!< Same value as CAP_FIREWIRE
	CAP_QT            = 500,          //!< QuickTime (obsolete, removed)
	CAP_UNICAP        = 600,          //!< Unicap drivers (obsolete, removed)
	CAP_DSHOW         = 700,          //!< DirectShow (via videoInput)
	CAP_PVAPI         = 800,          //!< PvAPI, Prosilica GigE SDK
	CAP_OPENNI        = 900,          //!< OpenNI (for Kinect)
	CAP_OPENNI_ASUS   = 910,          //!< OpenNI (for Asus Xtion)
	CAP_ANDROID       = 1000,         //!< MediaNDK (API Level 21+) and NDK Camera (API level 24+) for Android
	CAP_XIAPI         = 1100,         //!< XIMEA Camera API
	CAP_AVFOUNDATION  = 1200,         //!< AVFoundation framework for iOS (OS X Lion will have the same API)
	CAP_GIGANETIX     = 1300,         //!< Smartek Giganetix GigEVisionSDK
	CAP_MSMF          = 1400,         //!< Microsoft Media Foundation (via videoInput). See platform specific notes above.
	CAP_WINRT         = 1410,         //!< Microsoft Windows Runtime using Media Foundation
	CAP_INTELPERC     = 1500,         //!< RealSense (former Intel Perceptual Computing SDK)
	CAP_REALSENSE     = 1500,         //!< Synonym for CAP_INTELPERC
	CAP_OPENNI2       = 1600,         //!< OpenNI2 (for Kinect)
	CAP_OPENNI2_ASUS  = 1610,         //!< OpenNI2 (for Asus Xtion and Occipital Structure sensors)
	CAP_OPENNI2_ASTRA = 1620,         //!< OpenNI2 (for Orbbec Astra)
	CAP_GPHOTO2       = 1700,         //!< gPhoto2 connection
	CAP_GSTREAMER     = 1800,         //!< GStreamer
	CAP_FFMPEG        = 1900,         //!< Open and record video file or stream using the FFMPEG library
	CAP_IMAGES        = 2000,         //!< OpenCV Image Sequence (e.g. img_%02d.jpg)
	CAP_ARAVIS        = 2100,         //!< Aravis SDK
	CAP_OPENCV_MJPEG  = 2200,         //!< Built-in OpenCV MotionJPEG codec
	CAP_INTEL_MFX     = 2300,         //!< Intel MediaSDK
	CAP_XINE          = 2400,         //!< XINE engine (Linux)
	CAP_UEYE          = 2500,         //!< uEye Camera API
	CAP_OBSENSOR      = 2600,         //!< For Orbbec 3D-Sensor device/module (Astra+, Femto, Astra2, Gemini2, Gemini2L, Gemini2XL, Femto Mega) attention: Astra2 cameras currently only support Windows and Linux kernel versions no higher than 4.15, and higher versions of Linux kernel may have exceptions.
};

const char *to_str(const PixelFormat fmt);
const char *to_str(const Depth depth);
/// @brief convert color depth to size in bytes
/// @sa https://gist.github.com/yangcha/38f2fa630e223a8546f9b48ebbb3e61a
int size_of(Depth depth);
std::string_view to_string(const VideoCaptureAPIs api);
VideoCaptureAPIs from_string(std::string_view s);
PixelFormat guess_pixel_format(const int channels);
}