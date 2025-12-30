#ifndef B27BB190_CEA4_455B_ADF1_3716521874B0
#define B27BB190_CEA4_455B_ADF1_3716521874B0
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <format>
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
struct __attribute__((packed)) frame_info_t {
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

struct __attribute__((packed)) sync_message_t {
public:
	static constexpr auto LABEL_LEN_MAX = NAME_MAX_LEN;
	using label                         = uint8_t[NAME_MAX_LEN];
	struct __attribute__((packed)) attr {
		uint32_t frame_count;
	};

	sync_message_t(const std::string_view &label, uint32_t frame_count) : _attribute{.frame_count = frame_count} {
		if (label.size() > LABEL_LEN_MAX) {
			throw std::invalid_argument(std::format("label is too long: `{}`", label));
		}
		std::copy(label.begin(), label.end(), _label);
		std::fill(_label + label.size(), _label + LABEL_LEN_MAX, '\0');
	}

	sync_message_t &set_frame_count(uint32_t frame_count) {
		_attribute.frame_count = frame_count;
		return *this;
	}

	static constexpr size_t size() {
		return sizeof(_magic) + sizeof(sync_message_t);
	}

	/**
	 * @brief marshal the sync message to the buffer
	 * @param buf the buffer to marshal the sync message to
	 * @return the size of the marshaled sync message
	 * @note the buffer size must be at least `size()`
	 */
	int marshal(std::span<uint8_t> buf) const {
		if (buf.size() < size()) {
			return -1;
		}
		uint8_t *ptr    = buf.data();
		*ptr++          = _magic;
		const auto self = std::span<const uint8_t>{
			reinterpret_cast<const uint8_t *>(this), sizeof(sync_message_t)};
		std::copy(self.begin(), self.end(), ptr);
		return size();
	}

private:
	static constexpr uint8_t _magic = FRAME_TOPIC_MAGIC;
	/**
	 * @brief label of the video source
	 * @note C string, null-terminated
	 */
	attr _attribute;
	label _label;
};

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
	uint32_t frame_count;
	// TODO: uint64_t timestamp_ns;
	frame_info_t info;
};

}

#endif /* B27BB190_CEA4_455B_ADF1_3716521874B0 */
