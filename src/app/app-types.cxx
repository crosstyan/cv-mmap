module;

#include <cstdint>

export module app:types;
export namespace app {
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
}
