#ifndef A5270ABA_F5A6_4D0A_B75A_D89DADCC0FC3
#define A5270ABA_F5A6_4D0A_B75A_D89DADCC0FC3
#include <string_view>
#include <span>
#include <format>
#include "app_common_models.hpp"

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
				std::format("too long label: `{}`; {} > {}", label, label.size(), LABEL_LEN_MAX));
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
				std::format("too long label: `{}`; {} > {}", label, label.size(), LABEL_LEN_MAX));
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

}

#endif /* A5270ABA_F5A6_4D0A_B75A_D89DADCC0FC3 */
