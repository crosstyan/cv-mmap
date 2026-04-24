#include <cvmmap/parser.hpp>
#include <cvmmap/protocol.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace protocol = cvmmap::protocol;

constexpr uint16_t kWidth = 2;
constexpr uint16_t kHeight = 2;
constexpr uint32_t kLeftStrideBytes = 6;
constexpr uint32_t kLeftSizeBytes = kLeftStrideBytes * kHeight;
constexpr uint32_t kAuxStrideBytes = 8;
constexpr uint32_t kAuxSizeBytes = kAuxStrideBytes * kHeight;
constexpr uint32_t kEncodedSizeBytes = 6;

std::vector<uint8_t> make_payload(const bool include_depth, const bool include_confidence) {
	std::vector<uint8_t> payload;
	payload.reserve(
		kLeftSizeBytes + (include_depth ? kAuxSizeBytes : 0) +
		(include_confidence ? kAuxSizeBytes : 0));

	for (uint8_t i = 0; i < kLeftSizeBytes; ++i) {
		payload.push_back(static_cast<uint8_t>(0x10 + i));
	}
	if (include_depth) {
		for (uint8_t i = 0; i < kAuxSizeBytes; ++i) {
			payload.push_back(static_cast<uint8_t>(0x40 + i));
		}
	}
	if (include_confidence) {
		for (uint8_t i = 0; i < kAuxSizeBytes; ++i) {
			payload.push_back(static_cast<uint8_t>(0x70 + i));
		}
	}

	return payload;
}

std::vector<uint8_t> make_payload_with_encoded(
	const bool include_depth,
	const bool include_confidence) {
	auto payload = make_payload(include_depth, include_confidence);
	payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x01, 0x26, 0x01});
	return payload;
}

cvmmap::frame_plane_descriptor_v2_t make_left_descriptor() {
	return protocol::make_frame_metadata_v2_descriptor(
		cvmmap::FramePlaneType::Left,
		cvmmap::PixelFormat::BGR,
		cvmmap::Depth::U8,
		kWidth,
		kHeight,
		kLeftStrideBytes,
		0,
		kLeftSizeBytes);
}

cvmmap::frame_plane_descriptor_v2_t make_depth_descriptor() {
	return protocol::make_frame_metadata_v2_descriptor(
		cvmmap::FramePlaneType::Depth,
		cvmmap::PixelFormat::GRAY,
		cvmmap::Depth::F32,
		kWidth,
		kHeight,
		kAuxStrideBytes,
		kLeftSizeBytes,
		kAuxSizeBytes);
}

cvmmap::frame_plane_descriptor_v2_t make_confidence_descriptor() {
	return protocol::make_frame_metadata_v2_descriptor(
		cvmmap::FramePlaneType::Confidence,
		cvmmap::PixelFormat::GRAY,
		cvmmap::Depth::F32,
		kWidth,
		kHeight,
		kAuxStrideBytes,
		kLeftSizeBytes + kAuxSizeBytes,
		kAuxSizeBytes);
}

cvmmap::frame_metadata_v2_t require_metadata(
	cvmmap::expected<cvmmap::frame_metadata_v2_t, std::string> metadata) {
	if (!metadata) {
		throw std::runtime_error(metadata.error());
	}
	return *metadata;
}

cvmmap::frame_metadata_v2_t make_metadata_v2(
	const bool include_depth,
	const bool include_confidence,
	const cvmmap::DepthUnit depth_unit = cvmmap::DepthUnit::Unknown) {
	protocol::frame_metadata_v2_build_input_t input{
		.frame_id = 7,
		.capture_ts_ns = 123456789u,
		.publish_seq = 7,
		.depth_unit = depth_unit,
	};
	input.descriptors[0] = make_left_descriptor();
	if (include_depth) {
		input.descriptors[1] = make_depth_descriptor();
	}
	if (include_confidence) {
		input.descriptors[2] = make_confidence_descriptor();
	}
	return require_metadata(protocol::build_frame_metadata_v2(input));
}

cvmmap::frame_metadata_v2_t make_metadata_v2_with_encoded(
	const bool include_depth,
	const bool include_confidence,
	const cvmmap::EncodedCodec encoded_codec = cvmmap::EncodedCodec::H265,
	const cvmmap::EncodedBitstreamFormat bitstream_format =
		cvmmap::EncodedBitstreamFormat::AnnexB) {
	protocol::frame_metadata_v2_build_input_t input{
		.frame_id = 7,
		.capture_ts_ns = 123456789u,
		.publish_seq = 7,
		.depth_unit = cvmmap::DepthUnit::Millimeter,
	};
	input.descriptors[0] = make_left_descriptor();
	if (include_depth) {
		input.descriptors[1] = make_depth_descriptor();
	}
	if (include_confidence) {
		input.descriptors[2] = make_confidence_descriptor();
	}

	const auto encoded_offset = static_cast<uint32_t>(
		kLeftSizeBytes + (include_depth ? kAuxSizeBytes : 0) +
		(include_confidence ? kAuxSizeBytes : 0));
	input.descriptors[3] =
		protocol::make_encoded_access_unit_descriptor(encoded_offset, kEncodedSizeBytes);
	input.encoded_extension = cvmmap::frame_metadata_v2_encoded_extension_t{
		.encoded_codec = encoded_codec,
		.encoded_bitstream_format = bitstream_format,
		.encoded_flags = cvmmap::FRAME_METADATA_V2_ENCODED_FLAG_KEYFRAME,
		.encoded_frame_rate_num = 30,
		.encoded_frame_rate_den = 1,
		.encoded_stream_pts_ns = 987654321u,
	};
	return require_metadata(protocol::build_frame_metadata_v2(input));
}

std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> make_metadata_region(
	const cvmmap::frame_metadata_v2_t &metadata) {
	std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> metadata_region{};
	std::memcpy(metadata_region.data(), &metadata, sizeof(metadata));
	return metadata_region;
}

bool parse_fails_with(
	const cvmmap::frame_metadata_v2_t &metadata,
	const std::vector<uint8_t> &payload,
	const std::string &needle) {
	const auto metadata_region = make_metadata_region(metadata);
	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (parsed) {
		std::cerr << "expected parse failure containing `" << needle << "`\n";
		return false;
	}
	if (parsed.error().find(needle) == std::string::npos) {
		std::cerr << "expected error containing `" << needle << "`, got `"
				  << parsed.error() << "`\n";
		return false;
	}
	return true;
}

bool test_v2_left_only_parse() {
	const auto metadata = make_metadata_v2(false, false);
	const auto payload = make_payload(false, false);
	const auto metadata_region = make_metadata_region(metadata);

	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (!parsed) {
		std::cerr << "expected valid left-only packet, got error: " << parsed.error() << '\n';
		return false;
	}

	return parsed->normalized_metadata.versions_major == cvmmap::FRAME_METADATA_V2_MAJOR &&
		parsed->left_plane.size() == kLeftSizeBytes &&
		parsed->left_plane[0] == 0x10 &&
		parsed->depth_unit == cvmmap::DepthUnit::Unknown &&
		!parsed->depth_info.has_value() &&
		parsed->depth_plane.empty() &&
		!parsed->confidence_info.has_value() &&
		parsed->confidence_plane.empty();
}

bool test_v2_left_and_depth_parse() {
	const auto metadata = make_metadata_v2(true, false);
	const auto payload = make_payload(true, false);
	const auto metadata_region = make_metadata_region(metadata);

	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (!parsed) {
		std::cerr << "expected valid left+depth packet, got error: " << parsed.error() << '\n';
		return false;
	}

	return parsed->normalized_metadata.versions_major == cvmmap::FRAME_METADATA_V2_MAJOR &&
		parsed->left_plane.size() == kLeftSizeBytes &&
		parsed->left_plane[0] == 0x10 &&
		parsed->depth_unit == cvmmap::DepthUnit::Unknown &&
		parsed->depth_info.has_value() &&
		parsed->depth_info->pixel_format == cvmmap::PixelFormat::GRAY &&
		parsed->depth_plane.size() == kAuxSizeBytes &&
		parsed->depth_plane[0] == 0x40 &&
		!parsed->confidence_info.has_value() &&
		parsed->confidence_plane.empty();
}

bool test_v2_left_depth_and_confidence_parse() {
	const auto metadata = make_metadata_v2(true, true, cvmmap::DepthUnit::Millimeter);
	const auto payload = make_payload(true, true);
	const auto metadata_region = make_metadata_region(metadata);

	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (!parsed) {
		std::cerr << "expected valid left+depth+confidence packet, got error: " << parsed.error() << '\n';
		return false;
	}

	return parsed->depth_info.has_value() &&
		parsed->depth_unit == cvmmap::DepthUnit::Millimeter &&
		parsed->depth_plane.size() == kAuxSizeBytes &&
		parsed->confidence_info.has_value() &&
		parsed->confidence_info->pixel_format == cvmmap::PixelFormat::GRAY &&
		parsed->confidence_info->depth == cvmmap::Depth::F32 &&
		parsed->confidence_plane.size() == kAuxSizeBytes &&
		parsed->confidence_plane[0] == 0x70;
}

bool test_v2_depth_unit_meter_parse() {
	const auto metadata = make_metadata_v2(true, false, cvmmap::DepthUnit::Meter);
	const auto payload = make_payload(true, false);
	const auto metadata_region = make_metadata_region(metadata);

	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (!parsed) {
		std::cerr << "expected valid meter depth unit packet, got error: " << parsed.error() << '\n';
		return false;
	}

	return parsed->depth_unit == cvmmap::DepthUnit::Meter && parsed->depth_info.has_value();
}

bool test_v2_1_left_and_encoded_parse() {
	const auto metadata = make_metadata_v2_with_encoded(false, false);
	const auto payload = make_payload_with_encoded(false, false);
	const auto metadata_region = make_metadata_region(metadata);

	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (!parsed) {
		std::cerr << "expected valid left+encoded packet, got error: " << parsed.error() << '\n';
		return false;
	}

	return parsed->normalized_metadata.versions_minor == cvmmap::FRAME_METADATA_V2_MINOR_ENCODED_AU &&
		parsed->depth_plane.empty() &&
		parsed->confidence_plane.empty() &&
		parsed->encoded_codec == cvmmap::EncodedCodec::H265 &&
		parsed->encoded_bitstream_format == cvmmap::EncodedBitstreamFormat::AnnexB &&
		parsed->encoded_flags == cvmmap::FRAME_METADATA_V2_ENCODED_FLAG_KEYFRAME &&
		parsed->encoded_stream_pts_ns == 987654321u &&
		parsed->encoded_access_unit.size() == kEncodedSizeBytes &&
		parsed->encoded_access_unit[4] == 0x26;
}

bool test_v2_1_left_and_h264_encoded_parse() {
	const auto metadata = make_metadata_v2_with_encoded(
		false,
		false,
		cvmmap::EncodedCodec::H264);
	const auto payload = make_payload_with_encoded(false, false);
	const auto metadata_region = make_metadata_region(metadata);

	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (!parsed) {
		std::cerr << "expected valid left+h264-encoded packet, got error: " << parsed.error() << '\n';
		return false;
	}

	return parsed->normalized_metadata.versions_minor == cvmmap::FRAME_METADATA_V2_MINOR_ENCODED_AU &&
		parsed->encoded_codec == cvmmap::EncodedCodec::H264 &&
		parsed->encoded_bitstream_format == cvmmap::EncodedBitstreamFormat::AnnexB &&
		parsed->encoded_access_unit.size() == kEncodedSizeBytes;
}

bool test_v2_rejects_non_contiguous_base_mask() {
	auto metadata = make_metadata_v2(true, false);
	metadata.header.plane_presence_mask = 0x05;
	return parse_fails_with(
		metadata,
		make_payload(true, false),
		"does not match expected contiguous mask");
}

bool test_v2_1_rejects_mask_count_mismatch() {
	auto metadata = make_metadata_v2_with_encoded(false, false);
	metadata.header.plane_count = 3;
	return parse_fails_with(
		metadata,
		make_payload_with_encoded(false, false),
		"does not match popcount");
}

bool test_v2_rejects_inactive_descriptor() {
	auto metadata = make_metadata_v2(false, false);
	metadata.descriptors[1] = make_depth_descriptor();
	return parse_fails_with(
		metadata,
		make_payload(false, false),
		"must be empty when inactive");
}

bool test_v2_rejects_slot_type_mismatch() {
	auto metadata = make_metadata_v2(true, false);
	metadata.descriptors[1].plane_type = cvmmap::FramePlaneType::Confidence;
	return parse_fails_with(
		metadata,
		make_payload(true, false),
		"slot 1 must be DEPTH");
}

bool test_v2_rejects_descriptor_bounds() {
	auto metadata = make_metadata_v2(false, false);
	metadata.descriptors[0].size_bytes = metadata.header.payload_size_bytes + 1;
	return parse_fails_with(
		metadata,
		make_payload(false, false),
		"out of bounds");
}

bool test_v2_rejects_small_stride() {
	auto metadata = make_metadata_v2(false, false);
	metadata.descriptors[0].stride_bytes = 1;
	return parse_fails_with(
		metadata,
		make_payload(false, false),
		"stride=1 smaller than minimum");
}

bool test_v2_1_rejects_unknown_encoded_codec() {
	auto metadata = make_metadata_v2_with_encoded(false, false);
	auto extension = protocol::encoded_extension_from_header(metadata.header);
	extension.encoded_codec = cvmmap::EncodedCodec::Unknown;
	protocol::write_encoded_extension_to_header(metadata.header, extension);
	return parse_fails_with(
		metadata,
		make_payload_with_encoded(false, false),
		"requires a non-unknown encoded_codec");
}

bool test_v2_1_rejects_unknown_encoded_bitstream_format() {
	auto metadata = make_metadata_v2_with_encoded(false, false);
	auto extension = protocol::encoded_extension_from_header(metadata.header);
	extension.encoded_bitstream_format = cvmmap::EncodedBitstreamFormat::Unknown;
	protocol::write_encoded_extension_to_header(metadata.header, extension);
	return parse_fails_with(
		metadata,
		make_payload_with_encoded(false, false),
		"requires a non-unknown encoded_bitstream_format");
}

} // namespace

int main() {
	if (!test_v2_left_only_parse()) {
		std::cerr << "v2 left-only parse test failed\n";
		return 1;
	}
	if (!test_v2_left_and_depth_parse()) {
		std::cerr << "v2 left+depth parse test failed\n";
		return 1;
	}
	if (!test_v2_left_depth_and_confidence_parse()) {
		std::cerr << "v2 left+depth+confidence parse test failed\n";
		return 1;
	}
	if (!test_v2_depth_unit_meter_parse()) {
		std::cerr << "v2 meter depth unit parse test failed\n";
		return 1;
	}
	if (!test_v2_1_left_and_encoded_parse()) {
		std::cerr << "v2.1 left+encoded parse test failed\n";
		return 1;
	}
	if (!test_v2_1_left_and_h264_encoded_parse()) {
		std::cerr << "v2.1 left+h264-encoded parse test failed\n";
		return 1;
	}
	if (!test_v2_rejects_non_contiguous_base_mask()) {
		std::cerr << "v2 non-contiguous mask reject test failed\n";
		return 1;
	}
	if (!test_v2_1_rejects_mask_count_mismatch()) {
		std::cerr << "v2.1 mask count mismatch reject test failed\n";
		return 1;
	}
	if (!test_v2_rejects_inactive_descriptor()) {
		std::cerr << "v2 inactive descriptor reject test failed\n";
		return 1;
	}
	if (!test_v2_rejects_slot_type_mismatch()) {
		std::cerr << "v2 slot type mismatch reject test failed\n";
		return 1;
	}
	if (!test_v2_rejects_descriptor_bounds()) {
		std::cerr << "v2 descriptor bounds reject test failed\n";
		return 1;
	}
	if (!test_v2_rejects_small_stride()) {
		std::cerr << "v2 small stride reject test failed\n";
		return 1;
	}
	if (!test_v2_1_rejects_unknown_encoded_codec()) {
		std::cerr << "v2.1 unknown encoded codec reject test failed\n";
		return 1;
	}
	if (!test_v2_1_rejects_unknown_encoded_bitstream_format()) {
		std::cerr << "v2.1 unknown bitstream format reject test failed\n";
		return 1;
	}
	return 0;
}
