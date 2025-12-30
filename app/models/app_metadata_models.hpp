#ifndef B27BB190_CEA4_455B_ADF1_3716521874B0
#define B27BB190_CEA4_455B_ADF1_3716521874B0
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <format>
#include <string_view>
#include <sys/types.h>
#include <type_traits>
#include "app_enum_models.hpp"

namespace app {
constexpr auto NAME_MAX_LEN = 24;
/**
 * @brief offset of the shared memory payload
 * @note the first 256 bytes are reserved for the frame info and other useful metadata
 */
constexpr auto SHM_PAYLOAD_OFFSET = 256;
constexpr auto FRAME_TOPIC_MAGIC  = 0x7d;

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
	uint32_t buffer_size;
	PixelFormat pixel_format = PixelFormat::BGR;
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

struct sync_message_t {
public:
	static constexpr auto LABEL_LEN_MAX = NAME_MAX_LEN;

	sync_message_t() {
		_magic = FRAME_TOPIC_MAGIC;
		std::memset(_label, 0, LABEL_LEN_MAX);
	};
	sync_message_t(const std::string_view &label, uint32_t frame_count) : frame_count(frame_count) {
		_magic = FRAME_TOPIC_MAGIC;
		if (label.size() > LABEL_LEN_MAX) {
			throw std::invalid_argument(std::format("label is too long: `{}`", label));
		}
		std::copy(label.begin(), label.end(), _label);
		std::fill(_label + label.size(), _label + LABEL_LEN_MAX, '\0');
	}

	sync_message_t &set_frame_count(uint32_t frame_count) {
		auto atomic_ref = std::atomic_ref<uint32_t>(this->frame_count);
		atomic_ref.store(frame_count, std::memory_order::relaxed);
		return *this;
	}

	static constexpr size_t size() {
		return sizeof(sync_message_t);
	}

	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	std::span<const std::byte> as_bytes() const {
		return std::span<const std::byte>{
			reinterpret_cast<const std::byte *>(this), sizeof(sync_message_t)};
	}

	std::span<const uint8_t> as_uint8s() const {
		return std::span<const uint8_t>{
			reinterpret_cast<const uint8_t *>(this), sizeof(sync_message_t)};
	}

	/** properties */
	uint8_t _magic = FRAME_TOPIC_MAGIC;
	uint8_t _reserved_0[3]; // padding
	uint32_t frame_count;
	uint8_t _reserved_1[4]; // padding
	uint64_t timestamp_ns;
	/**
	 * @brief label of the video source
	 * @note C string, null-terminated
	 */
	uint8_t _label[NAME_MAX_LEN];
};
static_assert(std::alignment_of<sync_message_t>::value == 8, "sync_message_t must be 8-byte aligned");

/**
 * @note native aligned frame metadata
 */
struct frame_metadata_t {
	static constexpr auto CV_MMAP_MAGIC =
		std::array<char, 8>{'C', 'V', '-', 'M', 'M', 'A', 'P', '\0'};

	static constexpr auto size() {
		return CV_MMAP_MAGIC.size() + sizeof(frame_info_t);
	}

	/**
	 * @brief ensure the magic is set
	 */
	static bool ensure_magic(std::span<uint8_t> buf) {
		if (buf.size() < CV_MMAP_MAGIC.size()) {
			return false;
		}
		std::copy(CV_MMAP_MAGIC.begin(), CV_MMAP_MAGIC.end(), buf.begin());
		return true;
	}

	std::atomic_ref<uint32_t> frame_count_atomic() {
		return std::atomic_ref<uint32_t>(frame_count);
	};

	/** properties */
	uint8_t magic[CV_MMAP_MAGIC.size()];
	uint8_t versions_major;
	uint8_t versions_minor;
	uint8_t _reserved_0[2];
	uint32_t frame_count;
	uint64_t timestamp_ns;
	frame_info_t info;
};
static_assert(sizeof(frame_metadata_t) < SHM_PAYLOAD_OFFSET, "frame_metadata_t size must be less than SHM_PAYLOAD_OFFSET");
static_assert(std::alignment_of<frame_metadata_t>::value == 8, "frame_metadata_t must be 8-byte aligned");
}

#endif /* B27BB190_CEA4_455B_ADF1_3716521874B0 */
