#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace cvmmap {

constexpr size_t LABEL_LEN_MAX = 24;
constexpr size_t SHM_PAYLOAD_OFFSET = 256;

constexpr uint8_t FRAME_TOPIC_MAGIC = 0x7d;
constexpr uint8_t MODULE_STATUS_MAGIC = 0x5a;

constexpr uint8_t CONTROL_MESSAGE_REQUEST_MAGIC = 0x3c;
constexpr uint8_t CONTROL_MESSAGE_RESPONSE_MAGIC = 0x3d;

constexpr int32_t CONTROL_MSG_CMD_GENERIC = 0;
constexpr int32_t CONTROL_MSG_CMD_RESET_FRAME_COUNT = 0x1001;

constexpr int32_t CONTROL_RESPONSE_OK = 0;
constexpr int32_t CONTROL_RESPONSE_UNKNOWN_CMD = -1;
constexpr int32_t CONTROL_RESPONSE_ERROR = -2;
constexpr int32_t CONTROL_RESPONSE_INVALID_MAGIC = -3;
constexpr int32_t CONTROL_RESPONSE_INVALID_LABEL = -4;
constexpr int32_t CONTROL_RESPONSE_INVALID_VERSION = -5;
constexpr int32_t CONTROL_RESPONSE_INVALID_MSG_SIZE = -6;
constexpr int32_t CONTROL_RESPONSE_TIMEOUT = -100;

constexpr int32_t MODULE_STATUS_ONLINE = 0xa1;
constexpr int32_t MODULE_STATUS_OFFLINE = 0xa0;
constexpr int32_t MODULE_STATUS_STREAM_RESET = 0xb0;

constexpr uint8_t VERSION_MAJOR = 1;
constexpr uint8_t VERSION_MINOR = 0;

constexpr uint8_t FRAME_METADATA_V1_MAJOR = 1;
constexpr uint8_t FRAME_METADATA_V2_MAJOR = 2;

enum class PixelFormat : uint8_t {
	RGB = 0,
	BGR,
	RGBA,
	BGRA,
	GRAY,
	YUV,
	YUYV,
};

enum class Depth : uint8_t {
	U8 = 0,
	S8 = 1,
	U16 = 2,
	S16 = 3,
	S32 = 4,
	F32 = 5,
	F64 = 6,
	F16 = 7,
};

enum class FramePlaneType : uint8_t {
	Left = 0,
	Depth = 1,
	Confidence = 2,
};

enum class ModuleStatus : int32_t {
	Online = MODULE_STATUS_ONLINE,
	Offline = MODULE_STATUS_OFFLINE,
	StreamReset = MODULE_STATUS_STREAM_RESET,
};

constexpr int size_of(Depth depth) {
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
		return 0;
	}
}

struct frame_info_t {
	uint16_t width;
	uint16_t height;
	uint8_t channels;
	Depth depth;
	PixelFormat pixel_format;
	uint8_t _reserved_0[1];
	uint32_t buffer_size;

	[[nodiscard]]
	int pixelSize() const {
		return size_of(depth) * channels;
	}
};
static_assert(alignof(frame_info_t) == 4, "frame_info_t must be 4-byte aligned");
static_assert(sizeof(frame_info_t) == 12, "frame_info_t must be 12 bytes");

struct frame_metadata_t {
	static constexpr auto CV_MMAP_MAGIC =
		std::array<uint8_t, 8>{'C', 'V', '-', 'M', 'M', 'A', 'P', '\0'};

	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(magic)};
	}

	uint8_t magic[CV_MMAP_MAGIC.size()];
	uint8_t versions_major;
	uint8_t versions_minor;
	uint8_t _reserved_0[2];
	uint32_t frame_count;
	uint64_t timestamp_ns;
	frame_info_t info;
};
static_assert(sizeof(frame_metadata_t) < SHM_PAYLOAD_OFFSET,
			  "frame_metadata_t size must be less than SHM_PAYLOAD_OFFSET");
static_assert(alignof(frame_metadata_t) == 8,
			  "frame_metadata_t must be 8-byte aligned");

struct frame_plane_descriptor_v2_t {
	FramePlaneType plane_type;
	PixelFormat pixel_format;
	Depth depth;
	uint8_t _reserved_0{0};
	uint32_t width;
	uint32_t height;
	uint32_t stride_bytes;
	uint32_t offset_bytes;
	uint32_t size_bytes;
};
static_assert(sizeof(frame_plane_descriptor_v2_t) == 24,
			  "frame_plane_descriptor_v2_t must be 24 bytes");

struct frame_metadata_v2_header_t {
	uint8_t magic[frame_metadata_t::CV_MMAP_MAGIC.size()];
	uint8_t versions_major;
	uint8_t versions_minor;
	uint16_t flags;
	uint32_t frame_id;
	uint64_t capture_ts_ns;
	uint64_t publish_seq;
	uint8_t plane_count;
	uint8_t plane_presence_mask;
	uint16_t plane_descriptors_offset;
	uint16_t plane_descriptor_size;
	uint16_t plane_descriptor_capacity;
	uint32_t payload_size_bytes;
	uint8_t reserved_0[20];
};
static_assert(sizeof(frame_metadata_v2_header_t) == 64,
			  "frame_metadata_v2_header_t must be 64 bytes");

struct frame_metadata_v2_t {
	frame_metadata_v2_header_t header;
	std::array<frame_plane_descriptor_v2_t, 4> descriptors;
	uint8_t trailing_padding_to_payload[96];
};
static_assert(sizeof(frame_metadata_v2_t) == SHM_PAYLOAD_OFFSET,
			  "frame_metadata_v2_t must be 256 bytes");

struct frame_planes_view_t {
	std::span<const uint8_t> left{};
	std::optional<frame_info_t> depth_info{};
	std::span<const uint8_t> depth{};
	std::optional<frame_info_t> confidence_info{};
	std::span<const uint8_t> confidence{};
};

struct sync_message_t {
	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	[[nodiscard]]
	static constexpr size_t size() {
		return sizeof(sync_message_t);
	}

	uint8_t _magic{FRAME_TOPIC_MAGIC};
	uint8_t _reserved_0[1];
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	uint32_t frame_count;
	uint8_t _reserved_1[8];
	uint64_t timestamp_ns;
	uint8_t _label[LABEL_LEN_MAX];
};
static_assert(alignof(sync_message_t) == 8,
			  "sync_message_t must be 8-byte aligned");
static_assert(sizeof(sync_message_t) == 48, "sync_message_t must be 48 bytes");

struct module_status_message_t {
	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	[[nodiscard]]
	static constexpr size_t size() {
		return sizeof(module_status_message_t);
	}

	uint8_t _magic{MODULE_STATUS_MAGIC};
	uint8_t _reserved_0[1];
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	int32_t module_status;
	uint8_t _label[LABEL_LEN_MAX];
};
static_assert(sizeof(module_status_message_t) == 32,
			  "module_status_message_t must be 32 bytes");

struct control_message_request_t {
	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	void set_label(const std::string_view &label) {
		auto len = std::min(label.size(), LABEL_LEN_MAX - 1);
		std::copy(label.begin(), label.begin() + len, _label);
		std::fill(_label + len, _label + LABEL_LEN_MAX, '\0');
	}

	uint8_t _magic{CONTROL_MESSAGE_REQUEST_MAGIC};
	uint8_t _reserved_0[1];
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	int32_t command_id;
	uint8_t _label[LABEL_LEN_MAX];
	uint16_t request_message_length{0};
};
static_assert(sizeof(control_message_request_t) == 36,
			  "control_message_request_t must be 36 bytes");

struct control_message_response_t {
	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	uint8_t _magic{CONTROL_MESSAGE_RESPONSE_MAGIC};
	uint8_t _reserved_0[1];
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	int32_t command_id;
	int32_t response_code;
	uint8_t _label[LABEL_LEN_MAX];
	uint16_t response_message_length;
};
static_assert(sizeof(control_message_response_t) == 40,
			  "control_message_response_t must be 40 bytes");

} // namespace cvmmap
