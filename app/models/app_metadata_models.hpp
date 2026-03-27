#ifndef B27BB190_CEA4_455B_ADF1_3716521874B0
#define B27BB190_CEA4_455B_ADF1_3716521874B0
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <cvmmap/compat/format.hpp>
#include <string_view>
#include <sys/types.h>
#include <type_traits>
#include "app_enum_models.hpp"
#include "app_common_models.hpp"

namespace app {
struct sync_message_t {
	sync_message_t() {
		_magic         = FRAME_TOPIC_MAGIC;
		versions_major = VERSION_MAJOR;
		versions_minor = VERSION_MINOR;
		std::fill(std::begin(_label), std::end(_label), '\0');
	};
	sync_message_t(const std::string_view &label, uint32_t frame_count) : frame_count(frame_count) {
		_magic = FRAME_TOPIC_MAGIC;
		if (label.size() > LABEL_LEN_MAX) {
			throw std::invalid_argument(cvmmap::format("label is too long: `{}`", label));
		}
		std::copy(label.begin(), label.end(), _label);
		std::fill(_label + label.size(), _label + LABEL_LEN_MAX, '\0');
	}

	sync_message_t &set_frame_count(uint32_t frame_count) {
		auto atomic_ref = std::atomic_ref<uint32_t>(this->frame_count);
		atomic_ref.store(frame_count, std::memory_order::relaxed);
		return *this;
	}

	sync_message_t &set_timestamp_ns(uint64_t timestamp_ns) {
		auto atomic_ref = std::atomic_ref<uint64_t>(this->timestamp_ns);
		atomic_ref.store(timestamp_ns, std::memory_order::relaxed);
		return *this;
	}

	static constexpr size_t size() {
		return sizeof(sync_message_t);
	}

	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	std::span<const uint8_t> as_uint8s() const {
		return std::span<const uint8_t>{
			reinterpret_cast<const uint8_t *>(this), sizeof(sync_message_t)};
	}

	/** properties */
	uint8_t _magic{FRAME_TOPIC_MAGIC};
	uint8_t _reserved_0[1]; // padding
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	uint32_t frame_count;
	uint8_t _reserved_1[8]; // padding
	uint64_t timestamp_ns;
	/**
	 * @brief label of the video source
	 * @note C string, null-terminated
	 */
	uint8_t _label[LABEL_LEN_MAX];
};
static_assert(std::alignment_of<sync_message_t>::value == 8, "sync_message_t must be 8-byte aligned");

struct module_status_message_t {
	static constexpr size_t size() {
		return sizeof(module_status_message_t);
	}

	std::span<const uint8_t> as_uint8s() const {
		return std::span<const uint8_t>{
			reinterpret_cast<const uint8_t *>(this), sizeof(module_status_message_t)};
	}

	void _fill_label(const std::string_view &label) {
		if (label.size() > LABEL_LEN_MAX) {
			throw std::invalid_argument(cvmmap::format("label is too long: `{}`", label));
		}
		std::copy(label.begin(), label.end(), _label);
		std::fill(_label + label.size(), _label + LABEL_LEN_MAX, '\0');
	}

	void _fill_with_status(int32_t status, const std::string_view &label) {
		_magic         = MODULE_STATUS_MAGIC;
		versions_major = VERSION_MAJOR;
		versions_minor = VERSION_MINOR;
		module_status  = status;
		_fill_label(label);
	}

	static module_status_message_t make_online(const std::string_view &label) {
		module_status_message_t msg;
		msg._fill_with_status(MODULE_STATUS_ONLINE, label);
		return msg;
	}

	static module_status_message_t make_offline(const std::string_view &label) {
		module_status_message_t msg;
		msg._fill_with_status(MODULE_STATUS_OFFLINE, label);
		return msg;
	}

	static module_status_message_t make_frame_reset(const std::string_view &label) {
		module_status_message_t msg;
		msg._fill_with_status(MODULE_STATUS_STREAM_RESET, label);
		return msg;
	}

	/** properties */
	uint8_t _magic{MODULE_STATUS_MAGIC};
	uint8_t _reserved_0[1]; // padding
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	int32_t module_status;
	uint8_t _label[LABEL_LEN_MAX];
};

// https://docs.opencv.org/4.x/d3/d63/classcv_1_1Mat.html
// See `Detailed Description`
// strides for each dimension
// stride[0]=channel
// stride[1]=channel*cols
// stride[2]=channel*cols*rows
struct frame_info_t {
	/** properties */
	uint16_t width;
	uint16_t height;
	uint8_t channels;
	/// CV_8U, CV_8S, CV_16U, CV_16S, CV_16F, CV_32S, CV_32F, CV_64F
	Depth depth;
	PixelFormat pixel_format;
	uint8_t _reserved_0[1]; // padding
	uint32_t buffer_size;
	/** end of properties */

	/// @brief pixel size in bytes
	[[nodiscard]]
	int pixelSize() const {
		return size_of(depth) * channels;
	}

	int marshal(std::span<uint8_t> buf) const {
		if (buf.size() < sizeof(frame_info_t)) {
			return -1;
		}
		memcpy(buf.data(), this, sizeof(frame_info_t));
		return sizeof(frame_info_t);
	}

	static std::optional<frame_info_t> unmarshal(const std::span<const uint8_t> buf) {
		if (buf.size() < sizeof(frame_info_t)) {
			return std::nullopt;
		}
		frame_info_t info;
		memcpy(&info, buf.data(), sizeof(frame_info_t));
		return info;
	}
};
static_assert(std::alignment_of<frame_info_t>::value == 4, "frame_info_t must be 4-byte aligned");

/**
 * @note native aligned frame metadata
 */
struct frame_metadata_t {
	static constexpr auto CV_MMAP_MAGIC =
		std::array<uint8_t, 8>{'C', 'V', '-', 'M', 'M', 'A', 'P', '\0'};

	static constexpr auto size() {
		return CV_MMAP_MAGIC.size() + sizeof(frame_info_t);
	}

	/**
	 * @brief ensure the magic is set
	 */
	void ensure_magic() {
		std::copy(
			CV_MMAP_MAGIC.begin(),
			CV_MMAP_MAGIC.end(),
			magic);
	}

	std::atomic_ref<uint32_t> frame_count_atomic() {
		return std::atomic_ref<uint32_t>(frame_count);
	};

	std::atomic_ref<uint64_t> timestamp_ns_atomic() {
		return std::atomic_ref<uint64_t>(timestamp_ns);
	};

	/** properties */
	uint8_t magic[CV_MMAP_MAGIC.size()];
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	uint8_t _reserved_0[2];
	uint32_t frame_count;
	uint64_t timestamp_ns;
	frame_info_t info;
};
static_assert(sizeof(frame_metadata_t) < SHM_PAYLOAD_OFFSET, "frame_metadata_t size must be less than SHM_PAYLOAD_OFFSET");
static_assert(std::alignment_of<frame_metadata_t>::value == 8, "frame_metadata_t must be 8-byte aligned");

enum class FramePlaneType : uint8_t {
	LEFT  = 0,
	DEPTH = 1,
	CONFIDENCE = 2,
	ENCODED_ACCESS_UNIT = 3,
};

enum class EncodedCodec : uint8_t {
	UNKNOWN = 0,
	H264 = 1,
	H265 = 2,
};

enum class EncodedBitstreamFormat : uint8_t {
	UNKNOWN = 0,
	ANNEXB = 1,
};

constexpr uint16_t FRAME_METADATA_V2_ENCODED_FLAG_KEYFRAME = 0x0001u;

enum class DepthUnit : uint8_t {
	Unknown = 0,
	Millimeter = 1,
	Meter = 2,
};

#pragma pack(push, 1)
struct frame_plane_descriptor_v2_t {
	FramePlaneType plane_type{FramePlaneType::LEFT};
	PixelFormat pixel_format{PixelFormat::BGR};
	Depth depth{Depth::U8};
	uint8_t reserved_0{0};
	uint32_t width{0};
	uint32_t height{0};
	uint32_t stride_bytes{0};
	uint32_t offset_bytes{0};
	uint32_t size_bytes{0};

	[[nodiscard]]
	bool is_empty_descriptor() const {
		return width == 0 && height == 0 && stride_bytes == 0 && offset_bytes == 0 && size_bytes == 0;
	}
};

struct frame_metadata_v2_header_t {
	static constexpr std::array<uint8_t, 8> CV_MMAP_MAGIC = {'C', 'V', '-', 'M', 'M', 'A', 'P', '\0'};
	static constexpr uint8_t VERSION_MAJOR_V2 = 2;
	static constexpr uint8_t VERSION_MINOR_V2_BASE = 0;
	static constexpr uint8_t VERSION_MINOR_V2_ENCODED_AU = 1;
	static constexpr uint16_t PLANE_DESCRIPTORS_OFFSET = 64;
	static constexpr uint16_t PLANE_DESCRIPTOR_SIZE = 24;
	static constexpr uint16_t PLANE_DESCRIPTOR_CAPACITY = 4;

	void ensure_magic() {
		std::copy(CV_MMAP_MAGIC.begin(), CV_MMAP_MAGIC.end(), magic);
	}

	[[nodiscard]]
	uint8_t contiguous_mask_expected() const {
		return static_cast<uint8_t>((1u << plane_count) - 1u);
	}

	[[nodiscard]]
	bool contiguous_mask_valid() const {
		return plane_presence_mask == contiguous_mask_expected();
	}

	[[nodiscard]]
	bool uses_sparse_plane_mask_semantics() const {
		return versions_minor >= VERSION_MINOR_V2_ENCODED_AU;
	}

	uint8_t magic[CV_MMAP_MAGIC.size()];
	uint8_t versions_major{VERSION_MAJOR_V2};
	uint8_t versions_minor{VERSION_MINOR};
	uint16_t flags{0};
	uint32_t frame_id{0};
	uint64_t capture_ts_ns{0};
	uint64_t publish_seq{0};
	uint8_t plane_count{1};
	uint8_t plane_presence_mask{0x01};
	uint16_t plane_descriptors_offset{PLANE_DESCRIPTORS_OFFSET};
	uint16_t plane_descriptor_size{PLANE_DESCRIPTOR_SIZE};
	uint16_t plane_descriptor_capacity{PLANE_DESCRIPTOR_CAPACITY};
	uint32_t payload_size_bytes{0};
	DepthUnit depth_unit{DepthUnit::Unknown};
	uint8_t reserved_0[19]{};
};

struct frame_metadata_v2_encoded_extension_t {
	EncodedCodec encoded_codec{EncodedCodec::UNKNOWN};
	EncodedBitstreamFormat encoded_bitstream_format{EncodedBitstreamFormat::UNKNOWN};
	uint16_t encoded_flags{0};
	uint16_t encoded_frame_rate_num{0};
	uint16_t encoded_frame_rate_den{0};
	uint64_t encoded_stream_pts_ns{0};
	uint8_t reserved_0[3]{};
};

struct frame_metadata_v2_t {
	frame_metadata_v2_header_t header;
	frame_plane_descriptor_v2_t plane_descriptors[frame_metadata_v2_header_t::PLANE_DESCRIPTOR_CAPACITY];
	uint8_t trailing_padding_to_payload[96];
};
#pragma pack(pop)

static_assert(sizeof(frame_plane_descriptor_v2_t) == 24, "frame_plane_descriptor_v2_t must be 24 bytes");
static_assert(offsetof(frame_plane_descriptor_v2_t, plane_type) == 0, "plane_type offset must be 0");
static_assert(offsetof(frame_plane_descriptor_v2_t, pixel_format) == 1, "pixel_format offset must be 1");
static_assert(offsetof(frame_plane_descriptor_v2_t, depth) == 2, "depth offset must be 2");
static_assert(offsetof(frame_plane_descriptor_v2_t, reserved_0) == 3, "reserved_0 offset must be 3");
static_assert(offsetof(frame_plane_descriptor_v2_t, width) == 4, "width offset must be 4");
static_assert(offsetof(frame_plane_descriptor_v2_t, height) == 8, "height offset must be 8");
static_assert(offsetof(frame_plane_descriptor_v2_t, stride_bytes) == 12, "stride_bytes offset must be 12");
static_assert(offsetof(frame_plane_descriptor_v2_t, offset_bytes) == 16, "offset_bytes offset must be 16");
static_assert(offsetof(frame_plane_descriptor_v2_t, size_bytes) == 20, "size_bytes offset must be 20");
static_assert(alignof(frame_plane_descriptor_v2_t) == 1, "frame_plane_descriptor_v2_t must be packed");

static_assert(sizeof(frame_metadata_v2_header_t) == 64, "frame_metadata_v2_header_t must be 64 bytes");
static_assert(offsetof(frame_metadata_v2_header_t, magic) == 0x00, "magic offset must be 0x00");
static_assert(offsetof(frame_metadata_v2_header_t, versions_major) == 0x08, "versions_major offset must be 0x08");
static_assert(offsetof(frame_metadata_v2_header_t, versions_minor) == 0x09, "versions_minor offset must be 0x09");
static_assert(offsetof(frame_metadata_v2_header_t, flags) == 0x0A, "flags offset must be 0x0A");
static_assert(offsetof(frame_metadata_v2_header_t, frame_id) == 0x0C, "frame_id offset must be 0x0C");
static_assert(offsetof(frame_metadata_v2_header_t, capture_ts_ns) == 0x10, "capture_ts_ns offset must be 0x10");
static_assert(offsetof(frame_metadata_v2_header_t, publish_seq) == 0x18, "publish_seq offset must be 0x18");
static_assert(offsetof(frame_metadata_v2_header_t, plane_count) == 0x20, "plane_count offset must be 0x20");
static_assert(offsetof(frame_metadata_v2_header_t, plane_presence_mask) == 0x21, "plane_presence_mask offset must be 0x21");
static_assert(offsetof(frame_metadata_v2_header_t, plane_descriptors_offset) == 0x22, "plane_descriptors_offset offset must be 0x22");
static_assert(offsetof(frame_metadata_v2_header_t, plane_descriptor_size) == 0x24, "plane_descriptor_size offset must be 0x24");
static_assert(offsetof(frame_metadata_v2_header_t, plane_descriptor_capacity) == 0x26, "plane_descriptor_capacity offset must be 0x26");
static_assert(offsetof(frame_metadata_v2_header_t, payload_size_bytes) == 0x28, "payload_size_bytes offset must be 0x28");
static_assert(offsetof(frame_metadata_v2_header_t, depth_unit) == 0x2C, "depth_unit offset must be 0x2C");
static_assert(offsetof(frame_metadata_v2_header_t, reserved_0) == 0x2D, "reserved_0 offset must be 0x2D");
static_assert(alignof(frame_metadata_v2_header_t) == 1, "frame_metadata_v2_header_t must be packed");
static_assert(frame_metadata_v2_header_t::PLANE_DESCRIPTORS_OFFSET == 64, "v2 plane descriptors offset must be 64");
static_assert(frame_metadata_v2_header_t::PLANE_DESCRIPTOR_SIZE == 24, "v2 plane descriptor size must be 24");
static_assert(frame_metadata_v2_header_t::PLANE_DESCRIPTOR_CAPACITY == 4, "v2 plane descriptor capacity must be 4");
static_assert(sizeof(frame_metadata_v2_encoded_extension_t) == 19, "v2 encoded extension must fit reserved_0");

static_assert(sizeof(frame_metadata_v2_t) == SHM_PAYLOAD_OFFSET, "frame_metadata_v2_t must fully occupy metadata region");
static_assert(offsetof(frame_metadata_v2_t, header) == 0, "v2 header must start at offset 0");
static_assert(offsetof(frame_metadata_v2_t, plane_descriptors) == 64, "v2 descriptors must start at offset 64");
static_assert(offsetof(frame_metadata_v2_t, trailing_padding_to_payload) == 160, "v2 trailing padding must start at offset 160");
static_assert(alignof(frame_metadata_v2_t) == 1, "frame_metadata_v2_t must be packed");
}

#endif /* B27BB190_CEA4_455B_ADF1_3716521874B0 */
