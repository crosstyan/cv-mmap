#pragma once

#include "ipc.hpp"

#include <expected>
#include <optional>
#include <span>
#include <string>

namespace cvmmap {

struct parsed_frame_metadata_t {
	frame_metadata_t normalized_metadata{};
	std::span<const uint8_t> left_plane{};
	DepthUnit depth_unit{DepthUnit::Unknown};
	std::optional<frame_info_t> depth_info{};
	std::span<const uint8_t> depth_plane{};
	std::optional<frame_info_t> confidence_info{};
	std::span<const uint8_t> confidence_plane{};
};

std::expected<parsed_frame_metadata_t, std::string>
parse_frame_metadata_regions(std::span<const uint8_t> metadata_region,
							 std::span<const uint8_t> payload_region);

std::expected<body_tracking_frame_t, std::string>
parse_body_tracking_message(std::span<const uint8_t> message);

} // namespace cvmmap
