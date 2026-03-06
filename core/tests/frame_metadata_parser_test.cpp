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

std::vector<uint8_t> make_payload(const bool include_confidence) {
	std::vector<uint8_t> payload;
	payload.reserve(kLeftSizeBytes + kAuxSizeBytes + (include_confidence ? kAuxSizeBytes : 0));

	for (uint8_t i = 0; i < kLeftSizeBytes; ++i) {
		payload.push_back(static_cast<uint8_t>(0x10 + i));
	}
	for (uint8_t i = 0; i < kAuxSizeBytes; ++i) {
		payload.push_back(static_cast<uint8_t>(0x40 + i));
	}
	if (include_confidence) {
		for (uint8_t i = 0; i < kAuxSizeBytes; ++i) {
			payload.push_back(static_cast<uint8_t>(0x70 + i));
		}
	}

	return payload;
}

cvmmap::frame_metadata_v2_t make_metadata_v2(const bool include_confidence) {
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
	metadata.header.plane_count = include_confidence ? 3 : 2;
	metadata.header.plane_presence_mask = include_confidence ? 0x07 : 0x03;
	metadata.header.plane_descriptors_offset = 64;
	metadata.header.plane_descriptor_size = 24;
	metadata.header.plane_descriptor_capacity = 4;
	metadata.header.payload_size_bytes =
		kLeftSizeBytes + kAuxSizeBytes + (include_confidence ? kAuxSizeBytes : 0);

	auto &left = metadata.descriptors[0];
	left.plane_type = cvmmap::FramePlaneType::Left;
	left.pixel_format = cvmmap::PixelFormat::BGR;
	left.depth = cvmmap::Depth::U8;
	left.width = kWidth;
	left.height = kHeight;
	left.stride_bytes = kLeftStrideBytes;
	left.offset_bytes = 0;
	left.size_bytes = kLeftSizeBytes;

	auto &depth = metadata.descriptors[1];
	depth.plane_type = cvmmap::FramePlaneType::Depth;
	depth.pixel_format = cvmmap::PixelFormat::GRAY;
	depth.depth = cvmmap::Depth::F32;
	depth.width = kWidth;
	depth.height = kHeight;
	depth.stride_bytes = kAuxStrideBytes;
	depth.offset_bytes = kLeftSizeBytes;
	depth.size_bytes = kAuxSizeBytes;

	if (include_confidence) {
		auto &confidence = metadata.descriptors[2];
		confidence.plane_type = cvmmap::FramePlaneType::Confidence;
		confidence.pixel_format = cvmmap::PixelFormat::GRAY;
		confidence.depth = cvmmap::Depth::F32;
		confidence.width = kWidth;
		confidence.height = kHeight;
		confidence.stride_bytes = kAuxStrideBytes;
		confidence.offset_bytes = kLeftSizeBytes + kAuxSizeBytes;
		confidence.size_bytes = kAuxSizeBytes;
	}

	return metadata;
}

bool test_v2_left_and_depth_parse() {
	const auto metadata = make_metadata_v2(false);
	const auto payload = make_payload(false);
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
		parsed->depth_info.has_value() &&
		parsed->depth_info->pixel_format == cvmmap::PixelFormat::GRAY &&
		parsed->depth_plane.size() == kAuxSizeBytes &&
		parsed->depth_plane[0] == 0x40 &&
		!parsed->confidence_info.has_value() &&
		parsed->confidence_plane.empty();
}

bool test_v2_left_depth_and_confidence_parse() {
	const auto metadata = make_metadata_v2(true);
	const auto payload = make_payload(true);
	std::array<uint8_t, cvmmap::SHM_PAYLOAD_OFFSET> metadata_region{};
	std::memcpy(metadata_region.data(), &metadata, sizeof(metadata));

	const auto parsed = cvmmap::parse_frame_metadata_regions(metadata_region, payload);
	if (!parsed) {
		std::cerr << "expected valid left+depth+confidence packet, got error: " << parsed.error() << '\n';
		return false;
	}

	return parsed->depth_info.has_value() &&
		parsed->depth_plane.size() == kAuxSizeBytes &&
		parsed->confidence_info.has_value() &&
		parsed->confidence_info->pixel_format == cvmmap::PixelFormat::GRAY &&
		parsed->confidence_info->depth == cvmmap::Depth::F32 &&
		parsed->confidence_plane.size() == kAuxSizeBytes &&
		parsed->confidence_plane[0] == 0x70;
}

} // namespace

int main() {
	if (!test_v2_left_and_depth_parse()) {
		std::cerr << "v2 left+depth parse test failed\n";
		return 1;
	}
	if (!test_v2_left_depth_and_confidence_parse()) {
		std::cerr << "v2 left+depth+confidence parse test failed\n";
		return 1;
	}
	return 0;
}
