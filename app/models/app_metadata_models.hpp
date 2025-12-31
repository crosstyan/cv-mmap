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
	uint8_t _reserved_1[4]; // padding
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
			throw std::invalid_argument(std::format("label is too long: `{}`", label));
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
}

#endif /* B27BB190_CEA4_455B_ADF1_3716521874B0 */
