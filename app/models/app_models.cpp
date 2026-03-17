#include <stdexcept>
#include <cvmmap/compat/format.hpp>
#include <unordered_map>
#include "app_enum_models.hpp"

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

int size_of(Depth depth) {
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
		throw app::invalid_argument(cvmmap::format("invalid depth value `{}`", static_cast<int>(depth)));
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
	throw invalid_argument(cvmmap::format("invalid API value: `{}`", static_cast<int>(api)));
}

VideoCaptureAPIs from_string(const std::string_view s) {
	for (const auto &[key, value] : api_map) {
		if (key == s) {
			return value;
		}
	}
	throw invalid_argument(cvmmap::format("invalid API key: `{}`", s));
}

PixelFormat guess_pixel_format(const int channels) {
	switch (channels) {
	case 1:
		return PixelFormat::GRAY;
	case 3:
		return PixelFormat::BGR;
	case 4:
		return PixelFormat::BGRA;
	default:
		throw invalid_argument(cvmmap::format("invalid channel count: `{}`", channels));
	}
};
}