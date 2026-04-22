#include <cvmmap/parser.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

constexpr uint16_t kWidth = 2;
constexpr uint16_t kHeight = 2;
constexpr uint32_t kLeftStrideBytes = 6;
constexpr uint32_t kLeftSizeBytes = kLeftStrideBytes * kHeight;
constexpr uint32_t kAuxStrideBytes = 8;
constexpr uint32_t kAuxSizeBytes = kAuxStrideBytes * kHeight;

std::vector<uint8_t> make_payload(const bool include_depth, const bool include_confidence) {
	std::vector<uint8_t> payload;
	payload.reserve(kLeftSizeBytes + (include_depth ? kAuxSizeBytes : 0) + (include_confidence ? kAuxSizeBytes : 0));

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

std::vector<uint8_t> make_payload_with_encoded(const bool include_depth, const bool include_confidence) {
	std::vector<uint8_t> payload;
	payload.reserve(
		kLeftSizeBytes +
		(include_depth ? kAuxSizeBytes : 0) +
		(include_confidence ? kAuxSizeBytes : 0) +
		6);

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
	payload.insert(payload.end(), {0x00, 0x00, 0x00, 0x01, 0x26, 0x01});
	return payload;
}

cvmmap::frame_metadata_v2_t make_metadata_v2(
	const bool include_depth,
	const bool include_confidence,
	const cvmmap::DepthUnit depth_unit = cvmmap::DepthUnit::Unknown) {
	cvmmap::frame_metadata_v2_t metadata{};
	std::memcpy(
		metadata.header.magic,
		cvmmap::frame_metadata_t::CV_MMAP_MAGIC.data(),
		cvmmap::frame_metadata_t::CV_MMAP_MAGIC.size());
	metadata.header.versions_major = cvmmap::FRAME_METADATA_V2_MAJOR;
	metadata.header.versions_minor = cvmmap::VERSION_MINOR;
	metadata.header.frame_id = 7;
	metadata.header.capture_ts_ns = 123456789u;
	metadata.header.publish_seq = 7;
	metadata.header.plane_count = static_cast<uint8_t>(1 + (include_depth ? 1 : 0) + (include_confidence ? 1 : 0));
	metadata.header.plane_presence_mask = static_cast<uint8_t>(0x01 | (include_depth ? 0x02 : 0x00) | (include_confidence ? 0x04 : 0x00));
	metadata.header.plane_descriptors_offset = 64;
	metadata.header.plane_descriptor_size = 24;
	metadata.header.plane_descriptor_capacity = 4;
	metadata.header.payload_size_bytes = kLeftSizeBytes + (include_depth ? kAuxSizeBytes : 0) + (include_confidence ? kAuxSizeBytes : 0);
	metadata.header.depth_unit = depth_unit;

	auto &left = metadata.descriptors[0];
	left.plane_type = cvmmap::FramePlaneType::Left;
	left.pixel_format = cvmmap::PixelFormat::BGR;
	left.depth = cvmmap::Depth::U8;
	left.width = kWidth;
	left.height = kHeight;
	left.stride_bytes = kLeftStrideBytes;
	left.offset_bytes = 0;
	left.size_bytes = kLeftSizeBytes;

	if (include_depth) {
		auto &depth = metadata.descriptors[1];
		depth.plane_type = cvmmap::FramePlaneType::Depth;
		depth.pixel_format = cvmmap::PixelFormat::GRAY;
		depth.depth = cvmmap::Depth::F32;
		depth.width = kWidth;
		depth.height = kHeight;
		depth.stride_bytes = kAuxStrideBytes;
		depth.offset_bytes = kLeftSizeBytes;
		depth.size_bytes = kAuxSizeBytes;
	}

	if (include_confidence) {
		auto &confidence = metadata.descriptors[include_depth ? 2 : 1];
		confidence.plane_type = cvmmap::FramePlaneType::Confidence;
		confidence.pixel_format = cvmmap::PixelFormat::GRAY;
		confidence.depth = cvmmap::Depth::F32;
		confidence.width = kWidth;
		confidence.height = kHeight;
		confidence.stride_bytes = kAuxStrideBytes;
		confidence.offset_bytes = kLeftSizeBytes + (include_depth ? kAuxSizeBytes : 0);
		confidence.size_bytes = kAuxSizeBytes;
	}

	return metadata;
}

cvmmap::frame_metadata_v2_t make_metadata_v2_with_encoded(
	const bool include_depth,
	const bool include_confidence,
	const cvmmap::EncodedCodec encoded_codec = cvmmap::EncodedCodec::H265) {
	auto metadata = make_metadata_v2(include_depth, include_confidence, cvmmap::DepthUnit::Millimeter);
	metadata.header.versions_minor = cvmmap::FRAME_METADATA_V2_MINOR_ENCODED_AU;
	metadata.header.plane_presence_mask = static_cast<uint8_t>(
		0x01 |
		(include_depth ? 0x02 : 0x00) |
		(include_confidence ? 0x04 : 0x00) |
		0x08);
	metadata.header.plane_count = static_cast<uint8_t>(
		1 + (include_depth ? 1 : 0) + (include_confidence ? 1 : 0) + 1);

	const auto encoded_offset = static_cast<uint32_t>(
		kLeftSizeBytes +
		(include_depth ? kAuxSizeBytes : 0) +
		(include_confidence ? kAuxSizeBytes : 0));
	metadata.header.payload_size_bytes = encoded_offset + 6;

	cvmmap::frame_metadata_v2_encoded_extension_t encoded_ext{};
	encoded_ext.encoded_codec = encoded_codec;
	encoded_ext.encoded_bitstream_format = cvmmap::EncodedBitstreamFormat::AnnexB;
	encoded_ext.encoded_flags = cvmmap::FRAME_METADATA_V2_ENCODED_FLAG_KEYFRAME;
	encoded_ext.encoded_frame_rate_num = 30;
	encoded_ext.encoded_frame_rate_den = 1;
	encoded_ext.encoded_stream_pts_ns = 987654321u;
	std::memcpy(metadata.header.reserved_0, &encoded_ext, sizeof(encoded_ext));

	auto &encoded = metadata.descriptors[3];
	encoded.plane_type = cvmmap::FramePlaneType::EncodedAccessUnit;
	encoded.pixel_format = cvmmap::PixelFormat::GRAY;
	encoded.depth = cvmmap::Depth::U8;
	encoded.width = 6;
	encoded.height = 1;
	encoded.stride_bytes = 6;
	encoded.offset_bytes = encoded_offset;
	encoded.size_bytes = 6;

	if (!include_depth) {
		metadata.descriptors[1] = {};
	}
	if (!include_confidence) {
		metadata.descriptors[2] = {};
	}

	return metadata;
}

bool test_v2_left_only_parse() {
	const auto metadata = make_metadata_v2(false, false);
	const auto payload = make_payload(false, false);
	std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> metadata_region{};
	std::memcpy(metadata_region.data(), &metadata, sizeof(metadata));

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
	std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> metadata_region{};
	std::memcpy(metadata_region.data(), &metadata, sizeof(metadata));

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
	std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> metadata_region{};
	std::memcpy(metadata_region.data(), &metadata, sizeof(metadata));

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
	std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> metadata_region{};
	std::memcpy(metadata_region.data(), &metadata, sizeof(metadata));

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
	std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> metadata_region{};
	std::memcpy(metadata_region.data(), &metadata, sizeof(metadata));

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
		parsed->encoded_access_unit.size() == 6 &&
		parsed->encoded_access_unit[4] == 0x26;
}

bool test_v2_1_left_and_h264_encoded_parse() {
	const auto metadata = make_metadata_v2_with_encoded(false, false, cvmmap::EncodedCodec::H264);
	const auto payload = make_payload_with_encoded(false, false);
	std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> metadata_region{};
	std::memcpy(metadata_region.data(), &metadata, sizeof(metadata));

	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (!parsed) {
		std::cerr << "expected valid left+h264-encoded packet, got error: " << parsed.error() << '\n';
		return false;
	}

	return parsed->normalized_metadata.versions_minor == cvmmap::FRAME_METADATA_V2_MINOR_ENCODED_AU &&
		parsed->encoded_codec == cvmmap::EncodedCodec::H264 &&
		parsed->encoded_bitstream_format == cvmmap::EncodedBitstreamFormat::AnnexB &&
		parsed->encoded_access_unit.size() == 6;
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
	return 0;
}
