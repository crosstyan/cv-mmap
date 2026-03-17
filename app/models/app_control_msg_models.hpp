#ifndef A5270ABA_F5A6_4D0A_B75A_D89DADCC0FC3
#define A5270ABA_F5A6_4D0A_B75A_D89DADCC0FC3
#include <string_view>
#include <span>
#include <cvmmap/compat/format.hpp>
#include "app_common_models.hpp"
#include <cvmmap/ipc.hpp>

namespace app {
struct control_message_request_t {
	[[nodiscard]]
	std::span<const uint8_t> request_message() const {
		return std::span<const uint8_t>{
			_request_message_data, request_message_length};
	}

	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	void set_label(const std::string_view &label) {
		if (label.size() > LABEL_LEN_MAX) {
			throw std::invalid_argument(
				cvmmap::format("too long label: `{}`; {} > {}", label, label.size(), LABEL_LEN_MAX));
		}
		std::copy(label.begin(), label.end(), _label);
		std::fill(_label + label.size(), _label + LABEL_LEN_MAX, '\0');
	}

	/** properties */
	uint8_t _magic{CONTROL_MESSAGE_REQUEST_MAGIC};
	uint8_t _reserved_0[1]; // padding
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};

	int32_t command_id;
	/**
	 * @brief label of the video source
	 * @note C string, null-terminated
	 */
	uint8_t _label[LABEL_LEN_MAX];
	// flexible array member
	uint16_t request_message_length;
	uint8_t _request_message_data[];
};
static_assert(sizeof(control_message_request_t) == 36, "control_message_request_t must be 36 bytes");

struct control_message_response_t {
	[[nodiscard]]
	std::span<const uint8_t> response_message() const {
		return std::span<const uint8_t>{
			_response_message_data, response_message_length};
	}

	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	void set_label(const std::string_view &label) {
		if (label.size() > LABEL_LEN_MAX) {
			throw std::invalid_argument(
				cvmmap::format("too long label: `{}`; {} > {}", label, label.size(), LABEL_LEN_MAX));
		}
		std::copy(label.begin(), label.end(), _label);
		std::fill(_label + label.size(), _label + LABEL_LEN_MAX, '\0');
	}

	/** properties */
	uint8_t _magic{CONTROL_MESSAGE_RESPONSE_MAGIC};
	uint8_t _reserved_0[1]; // padding
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};

	int32_t command_id;
	int32_t response_code;
	/**
	 * @brief label of the video source
	 * @note C string, null-terminated
	 */
	uint8_t _label[LABEL_LEN_MAX];
	// flexible array member
	uint16_t response_message_length;
	uint8_t _response_message_data[];
};
static_assert(sizeof(control_message_response_t) == 40, "control_message_response_t must be 40 bytes");

#pragma pack(push, 1)
struct source_info_response_v1_t {
	uint16_t struct_size{sizeof(source_info_response_v1_t)};
	cvmmap::SourceKind source_kind{cvmmap::SourceKind::Unknown};
	cvmmap::TimestampDomain timestamp_domain{cvmmap::TimestampDomain::Unknown};
	uint32_t flags{0};
	uint64_t timeline_start_ns{0};
	uint64_t timeline_end_ns{0};
	uint64_t duration_ns{0};
	uint64_t current_timestamp_ns{0};
	uint32_t current_frame_count{0};
	uint32_t reserved_0{0};
};
static_assert(sizeof(source_info_response_v1_t) == 48, "source_info_response_v1_t must be 48 bytes");

struct seek_timestamp_request_v1_t {
	uint16_t struct_size{sizeof(seek_timestamp_request_v1_t)};
	uint16_t reserved_0{0};
	uint64_t target_timestamp_ns{0};
};
static_assert(sizeof(seek_timestamp_request_v1_t) == 12, "seek_timestamp_request_v1_t must be 12 bytes");

struct seek_timestamp_response_v1_t {
	uint16_t struct_size{sizeof(seek_timestamp_response_v1_t)};
	uint8_t exact_match{0};
	uint8_t reserved_0{0};
	uint64_t requested_timestamp_ns{0};
	uint64_t landed_timestamp_ns{0};
	uint32_t landed_frame_count{0};
	uint32_t reserved_1{0};
};
static_assert(sizeof(seek_timestamp_response_v1_t) == 28, "seek_timestamp_response_v1_t must be 28 bytes");

struct recording_start_request_v1_t {
	uint16_t struct_size{sizeof(recording_start_request_v1_t)};
	uint16_t flags{0};
	uint16_t path_length{0};
	uint16_t reserved_0{0};
};
static_assert(sizeof(recording_start_request_v1_t) == 8, "recording_start_request_v1_t must be 8 bytes");

struct recording_status_response_v1_t {
	uint16_t struct_size{sizeof(recording_status_response_v1_t)};
	cvmmap::RecordingFormat recording_format{cvmmap::RecordingFormat::Unknown};
	uint8_t reserved_0{0};
	uint16_t flags{0};
	uint16_t path_length{0};
	uint32_t frames_ingested{0};
	uint32_t frames_encoded{0};
	uint32_t reserved_1{0};
};
static_assert(sizeof(recording_status_response_v1_t) == 20, "recording_status_response_v1_t must be 20 bytes");
#pragma pack(pop)

}

#endif /* A5270ABA_F5A6_4D0A_B75A_D89DADCC0FC3 */
