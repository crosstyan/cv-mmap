#pragma once

#include "ipc.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>

#include <cvmmap/compat/expected.hpp>

namespace cvmmap::protocol {

inline constexpr uint16_t FRAME_METADATA_V2_DESCRIPTORS_OFFSET = 64;
inline constexpr uint16_t FRAME_METADATA_V2_DESCRIPTOR_SIZE = 24;
inline constexpr uint16_t FRAME_METADATA_V2_DESCRIPTOR_CAPACITY = 4;
inline constexpr uint8_t FRAME_METADATA_V2_MAX_PLANE_COUNT = 4;
inline constexpr uint8_t FRAME_METADATA_V2_VALID_PLANE_MASK = 0x0F;

struct frame_metadata_v2_build_input_t {
	uint32_t frame_id{0};
	uint64_t capture_ts_ns{0};
	uint64_t publish_seq{0};
	DepthUnit depth_unit{DepthUnit::Unknown};
	std::array<std::optional<frame_plane_descriptor_v2_t>, FRAME_METADATA_V2_DESCRIPTOR_CAPACITY>
		descriptors{};
	std::optional<frame_metadata_v2_encoded_extension_t> encoded_extension{};
};

void ensure_frame_metadata_v2_magic(frame_metadata_v2_header_t &header);
[[nodiscard]] uint8_t popcount_u8(uint8_t value);
[[nodiscard]] bool is_frame_metadata_v2_descriptor_empty(
	const frame_plane_descriptor_v2_t &descriptor);
[[nodiscard]] bool is_frame_metadata_v2_slot_active(
	const frame_metadata_v2_header_t &header,
	size_t slot);

[[nodiscard]] frame_metadata_v2_encoded_extension_t encoded_extension_from_header(
	const frame_metadata_v2_header_t &header);
void write_encoded_extension_to_header(
	frame_metadata_v2_header_t &header,
	const frame_metadata_v2_encoded_extension_t &extension);

[[nodiscard]] cvmmap::expected<frame_info_t, std::string>
frame_info_from_v2_descriptor(const frame_plane_descriptor_v2_t &descriptor);

[[nodiscard]] frame_plane_descriptor_v2_t make_frame_metadata_v2_descriptor(
	FramePlaneType plane_type,
	PixelFormat pixel_format,
	Depth depth,
	uint32_t width,
	uint32_t height,
	uint32_t stride_bytes,
	uint32_t offset_bytes,
	uint32_t size_bytes);

[[nodiscard]] frame_plane_descriptor_v2_t make_encoded_access_unit_descriptor(
	uint32_t offset_bytes,
	uint32_t size_bytes);

[[nodiscard]] cvmmap::expected<frame_metadata_v2_t, std::string>
build_frame_metadata_v2(const frame_metadata_v2_build_input_t &input);

[[nodiscard]] cvmmap::expected<void, std::string>
validate_frame_metadata_v2(
	const frame_metadata_v2_t &metadata,
	size_t payload_region_size);

} // namespace cvmmap::protocol
