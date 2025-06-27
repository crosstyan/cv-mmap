#include <app_models.hpp>

namespace app {
using invalid_argument = std::invalid_argument;
const char *to_str(const Depth depth) {
	switch (depth) {
	case Depth::U8:
		return "U8";
	case Depth::S8:
		return "S8";
	case Depth::U16:
		return "U16";
	case Depth::S16:
		return "S16";
	case Depth::F16:
		return "F16";
	case Depth::S32:
		return "S32";
	case Depth::F32:
		return "F32";
	case Depth::F64:
		return "F64";
	default:
		return "unknown";
	}
}

const char *to_str(const int depth) {
	return to_str(static_cast<Depth>(depth));
}

const char *to_str(const PixelFormat fmt) {
	switch (fmt) {
	case PixelFormat::RGB:
		return "RGB";
	case PixelFormat::BGR:
		return "BGR";
	case PixelFormat::RGBA:
		return "RGBA";
	case PixelFormat::BGRA:
		return "BGRA";
	case PixelFormat::GRAY:
		return "GRAY";
	case PixelFormat::YUV:
		return "YUV";
	case PixelFormat::YUYV:
		return "YUYV";
	default:
		return "unknown";
	}
}

/// @brief convert color depth to size in bytes
/// @sa https://gist.github.com/yangcha/38f2fa630e223a8546f9b48ebbb3e61a
inline int depth_to_size(Depth depth) {
	switch (depth) {
	case Depth::U8:
	case Depth::S8:
		return 1;
	case Depth::U16:
	case Depth::S16:
	case Depth::F16:
		return 2;
	case Depth::S32:
	case Depth::F32:
		return 4;
	case Depth::F64:
		return 8;
	default:
		throw app::invalid_argument(std::format("invalid depth value `{}`", static_cast<int>(depth)));
	}
}

static const std::unordered_map<std::string, VideoCaptureAPIs> api_map = {
	{"any", CAP_ANY},
	{"v4l", CAP_V4L},
	{"v4l2", CAP_V4L2},
	{"gstreamer", CAP_GSTREAMER},
	{"dshow", CAP_DSHOW},
	{"avfoundation", CAP_AVFOUNDATION},
	{"ffmpeg", CAP_FFMPEG},
};

std::string_view to_string(const VideoCaptureAPIs api) {
	for (const auto &[key, value] : api_map) {
		if (value == api) {
			return key;
		}
	}
	throw invalid_argument(std::format("invalid API value: `{}`", static_cast<int>(api)));
}

VideoCaptureAPIs from_string(const std::string_view s) {
	for (const auto &[key, value] : api_map) {
		if (key == s) {
			return value;
		}
	}
	throw invalid_argument(std::format("invalid API key: `{}`", s));
}
}