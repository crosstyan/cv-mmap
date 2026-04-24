#include <cvmmap/protocol.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

#include <cvmmap/compat/format.hpp>

namespace cvmmap::protocol {

namespace {

constexpr uint8_t channels_from_pixel_format(PixelFormat pixel_format) {
	switch (pixel_format) {
	case PixelFormat::RGB:
	case PixelFormat::BGR:
	case PixelFormat::YUV:
		return 3;
	case PixelFormat::RGBA:
	case PixelFormat::BGRA:
		return 4;
	case PixelFormat::GRAY:
		return 1;
	case PixelFormat::YUYV:
		return 2;
	default:
		return 0;
	}
}

constexpr size_t bytes_per_channel_from_depth(Depth depth) {
	const auto bytes = size_of(depth);
	if (bytes <= 0) {
		return 0;
	}
	return static_cast<size_t>(bytes);
}

constexpr bool is_supported_depth_unit(const DepthUnit unit) {
	switch (unit) {
	case DepthUnit::Unknown:
	case DepthUnit::Millimeter:
	case DepthUnit::Meter:
		return true;
	default:
		return false;
	}
}

constexpr bool is_supported_encoded_codec(const EncodedCodec codec) {
	switch (codec) {
	case EncodedCodec::Unknown:
	case EncodedCodec::H264:
	case EncodedCodec::H265:
		return true;
	default:
		return false;
	}
}

constexpr bool is_supported_encoded_bitstream_format(
	const EncodedBitstreamFormat format) {
	switch (format) {
	case EncodedBitstreamFormat::Unknown:
	case EncodedBitstreamFormat::AnnexB:
		return true;
	default:
		return false;
	}
}

} // namespace

void ensure_frame_metadata_v2_magic(frame_metadata_v2_header_t &header) {
	std::copy(
		frame_metadata_t::CV_MMAP_MAGIC.begin(),
		frame_metadata_t::CV_MMAP_MAGIC.end(),
		header.magic);
}

uint8_t popcount_u8(const uint8_t value) {
	uint8_t count = 0;
	for (uint8_t bits = value; bits != 0; bits >>= 1) {
		count += static_cast<uint8_t>(bits & 0x01u);
	}
	return count;
}

bool is_frame_metadata_v2_descriptor_empty(
	const frame_plane_descriptor_v2_t &descriptor) {
	return descriptor.width == 0 && descriptor.height == 0 &&
		   descriptor.stride_bytes == 0 && descriptor.offset_bytes == 0 &&
		   descriptor.size_bytes == 0;
}

bool is_frame_metadata_v2_slot_active(
	const frame_metadata_v2_header_t &header,
	const size_t slot) {
	return header.versions_minor == FRAME_METADATA_V2_MINOR_BASE
		? slot < header.plane_count
		: ((header.plane_presence_mask & (1u << slot)) != 0);
}

frame_metadata_v2_encoded_extension_t encoded_extension_from_header(
	const frame_metadata_v2_header_t &header) {
	frame_metadata_v2_encoded_extension_t extension{};
	std::memcpy(&extension, header.reserved_0, sizeof(extension));
	return extension;
}

void write_encoded_extension_to_header(
	frame_metadata_v2_header_t &header,
	const frame_metadata_v2_encoded_extension_t &extension) {
	std::memcpy(header.reserved_0, &extension, sizeof(extension));
}

cvmmap::expected<frame_info_t, std::string>
frame_info_from_v2_descriptor(const frame_plane_descriptor_v2_t &descriptor) {
	if (descriptor.width > std::numeric_limits<uint16_t>::max()) {
		return cvmmap::unexpected(cvmmap::format(
			"plane width {} exceeds frame_info_t limit",
			descriptor.width));
	}
	if (descriptor.height > std::numeric_limits<uint16_t>::max()) {
		return cvmmap::unexpected(cvmmap::format(
			"plane height {} exceeds frame_info_t limit",
			descriptor.height));
	}
	const auto channels = channels_from_pixel_format(descriptor.pixel_format);
	if (channels == 0) {
		return cvmmap::unexpected(cvmmap::format(
			"unsupported pixel_format={} in v2 descriptor",
			static_cast<uint8_t>(descriptor.pixel_format)));
	}

	frame_info_t info{};
	info.width = static_cast<uint16_t>(descriptor.width);
	info.height = static_cast<uint16_t>(descriptor.height);
	info.channels = channels;
	info.depth = descriptor.depth;
	info.pixel_format = descriptor.pixel_format;
	info.buffer_size = descriptor.size_bytes;
	return info;
}

frame_plane_descriptor_v2_t make_frame_metadata_v2_descriptor(
	const FramePlaneType plane_type,
	const PixelFormat pixel_format,
	const Depth depth,
	const uint32_t width,
	const uint32_t height,
	const uint32_t stride_bytes,
	const uint32_t offset_bytes,
	const uint32_t size_bytes) {
	return frame_plane_descriptor_v2_t{
		.plane_type = plane_type,
		.pixel_format = pixel_format,
		.depth = depth,
		._reserved_0 = 0,
		.width = width,
		.height = height,
		.stride_bytes = stride_bytes,
		.offset_bytes = offset_bytes,
		.size_bytes = size_bytes,
	};
}

frame_plane_descriptor_v2_t make_encoded_access_unit_descriptor(
	const uint32_t offset_bytes,
	const uint32_t size_bytes) {
	return make_frame_metadata_v2_descriptor(
		FramePlaneType::EncodedAccessUnit,
		PixelFormat::GRAY,
		Depth::U8,
		size_bytes,
		1,
		size_bytes,
		offset_bytes,
		size_bytes);
}

cvmmap::expected<frame_metadata_v2_t, std::string>
build_frame_metadata_v2(const frame_metadata_v2_build_input_t &input) {
	if (input.encoded_extension && !input.descriptors[3]) {
		return cvmmap::unexpected(
			"v2.1 encoded extension requires slot 3 descriptor");
	}

	frame_metadata_v2_t metadata{};
	auto &header = metadata.header;
	ensure_frame_metadata_v2_magic(header);
	header.versions_major = FRAME_METADATA_V2_MAJOR;
	header.versions_minor = input.descriptors[3]
		? FRAME_METADATA_V2_MINOR_ENCODED_AU
		: FRAME_METADATA_V2_MINOR_BASE;
	header.frame_id = input.frame_id;
	header.capture_ts_ns = input.capture_ts_ns;
	header.publish_seq = input.publish_seq;
	header.plane_descriptors_offset = FRAME_METADATA_V2_DESCRIPTORS_OFFSET;
	header.plane_descriptor_size = FRAME_METADATA_V2_DESCRIPTOR_SIZE;
	header.plane_descriptor_capacity = FRAME_METADATA_V2_DESCRIPTOR_CAPACITY;
	header.depth_unit = input.descriptors[1] ? input.depth_unit : DepthUnit::Unknown;

	uint8_t plane_count = 0;
	uint8_t presence_mask = 0;
	uint32_t payload_size = 0;
	for (size_t slot = 0; slot < input.descriptors.size(); ++slot) {
		const auto &descriptor = input.descriptors[slot];
		if (!descriptor) {
			continue;
		}
		metadata.descriptors[slot] = *descriptor;
		plane_count += 1;
		presence_mask |= static_cast<uint8_t>(1u << slot);

		const uint64_t descriptor_end =
			static_cast<uint64_t>(descriptor->offset_bytes) + descriptor->size_bytes;
		if (descriptor_end > std::numeric_limits<uint32_t>::max()) {
			return cvmmap::unexpected("v2 payload_size_bytes exceeds ABI limits");
		}
		payload_size = std::max(payload_size, static_cast<uint32_t>(descriptor_end));
	}

	header.plane_count = plane_count;
	header.plane_presence_mask = presence_mask;
	header.payload_size_bytes = payload_size;
	if (input.descriptors[3]) {
		write_encoded_extension_to_header(
			header,
			input.encoded_extension.value_or(
				frame_metadata_v2_encoded_extension_t{}));
	}

	auto validation = validate_frame_metadata_v2(metadata, payload_size);
	if (!validation) {
		return cvmmap::unexpected(validation.error());
	}
	return metadata;
}

cvmmap::expected<void, std::string> validate_frame_metadata_v2(
	const frame_metadata_v2_t &metadata,
	const size_t payload_region_size) {
	const auto &header = metadata.header;

	if (header.plane_count < 1 || header.plane_count > FRAME_METADATA_V2_MAX_PLANE_COUNT) {
		return cvmmap::unexpected(cvmmap::format(
			"v2 plane_count={} out of bounds [1, {}]",
			header.plane_count,
			FRAME_METADATA_V2_MAX_PLANE_COUNT));
	}
	if ((header.plane_presence_mask & ~FRAME_METADATA_V2_VALID_PLANE_MASK) != 0) {
		return cvmmap::unexpected(cvmmap::format(
			"v2 plane_presence_mask=0x{:02x} has invalid upper bits",
			header.plane_presence_mask));
	}
	if (header.versions_minor == FRAME_METADATA_V2_MINOR_BASE) {
		const auto expected_mask =
			static_cast<uint8_t>((1u << header.plane_count) - 1u);
		if (header.plane_presence_mask != expected_mask) {
			return cvmmap::unexpected(cvmmap::format(
				"v2 plane_presence_mask=0x{:02x} does not match expected "
				"contiguous mask 0x{:02x}",
				header.plane_presence_mask,
				expected_mask));
		}
	} else {
		if ((header.plane_presence_mask & 0x01u) == 0) {
			return cvmmap::unexpected(
				"v2.1 plane_presence_mask must include slot 0 LEFT plane");
		}
		const auto expected_plane_count = popcount_u8(header.plane_presence_mask);
		if (header.plane_count != expected_plane_count) {
			return cvmmap::unexpected(cvmmap::format(
				"v2.1 plane_count={} does not match popcount(mask)={}",
				header.plane_count,
				expected_plane_count));
		}
	}
	if (header.plane_descriptors_offset != FRAME_METADATA_V2_DESCRIPTORS_OFFSET) {
		return cvmmap::unexpected(cvmmap::format(
			"v2 plane_descriptors_offset={} expected {}",
			header.plane_descriptors_offset,
			FRAME_METADATA_V2_DESCRIPTORS_OFFSET));
	}
	if (header.plane_descriptor_size != FRAME_METADATA_V2_DESCRIPTOR_SIZE) {
		return cvmmap::unexpected(cvmmap::format(
			"v2 plane_descriptor_size={} expected {}",
			header.plane_descriptor_size,
			FRAME_METADATA_V2_DESCRIPTOR_SIZE));
	}
	if (header.plane_descriptor_capacity != FRAME_METADATA_V2_DESCRIPTOR_CAPACITY) {
		return cvmmap::unexpected(cvmmap::format(
			"v2 plane_descriptor_capacity={} expected {}",
			header.plane_descriptor_capacity,
			FRAME_METADATA_V2_DESCRIPTOR_CAPACITY));
	}
	if (header.payload_size_bytes == 0) {
		return cvmmap::unexpected("v2 payload_size_bytes must be non-zero");
	}
	if (!is_supported_depth_unit(header.depth_unit)) {
		return cvmmap::unexpected(cvmmap::format(
			"v2 depth_unit={} is unsupported",
			static_cast<uint8_t>(header.depth_unit)));
	}
	if (header.payload_size_bytes > payload_region_size) {
		return cvmmap::unexpected(cvmmap::format(
			"v2 payload_size_bytes={} exceeds shared payload capacity {}",
			header.payload_size_bytes,
			payload_region_size));
	}

	const auto encoded_extension = encoded_extension_from_header(header);
	if (header.versions_minor >= FRAME_METADATA_V2_MINOR_ENCODED_AU) {
		if (!is_supported_encoded_codec(encoded_extension.encoded_codec)) {
			return cvmmap::unexpected(cvmmap::format(
				"v2.1 encoded_codec={} is unsupported",
				static_cast<uint8_t>(encoded_extension.encoded_codec)));
		}
		if (!is_supported_encoded_bitstream_format(
				encoded_extension.encoded_bitstream_format)) {
			return cvmmap::unexpected(cvmmap::format(
				"v2.1 encoded_bitstream_format={} is unsupported",
				static_cast<uint8_t>(encoded_extension.encoded_bitstream_format)));
		}
	}

	uint32_t expected_next_offset = 0;
	for (size_t slot = 0; slot < metadata.descriptors.size(); ++slot) {
		const auto &descriptor = metadata.descriptors[slot];
		const bool is_active = is_frame_metadata_v2_slot_active(header, slot);
		const bool is_empty =
			is_frame_metadata_v2_descriptor_empty(descriptor);

		if (is_active) {
			if (is_empty) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} is active but empty",
					slot));
			}
			if (descriptor.width == 0 || descriptor.height == 0 ||
				descriptor.stride_bytes == 0 || descriptor.size_bytes == 0) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} has zero geometric/size field",
					slot));
			}
			if (descriptor.offset_bytes > header.payload_size_bytes) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} offset={} exceeds payload_size_bytes={}",
					slot,
					descriptor.offset_bytes,
					header.payload_size_bytes));
			}
			if (descriptor.size_bytes >
				(header.payload_size_bytes - descriptor.offset_bytes)) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} size={} out of bounds for "
					"payload_size_bytes={} and offset={}",
					slot,
					descriptor.size_bytes,
					header.payload_size_bytes,
					descriptor.offset_bytes));
			}

			const auto channels = channels_from_pixel_format(descriptor.pixel_format);
			if (channels == 0) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} has unsupported pixel_format={}",
					slot,
					static_cast<uint8_t>(descriptor.pixel_format)));
			}

			const auto bytes_per_channel =
				bytes_per_channel_from_depth(descriptor.depth);
			if (bytes_per_channel == 0) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} has unsupported depth={}",
					slot,
					static_cast<uint8_t>(descriptor.depth)));
			}

			const uint64_t min_stride_bytes =
				static_cast<uint64_t>(descriptor.width) *
				static_cast<uint64_t>(channels) *
				static_cast<uint64_t>(bytes_per_channel);
			if (static_cast<uint64_t>(descriptor.stride_bytes) < min_stride_bytes) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} stride={} smaller than minimum "
					"{} for width/channels/depth",
					slot,
					descriptor.stride_bytes,
					min_stride_bytes));
			}

			const uint64_t min_size_bytes =
				static_cast<uint64_t>(descriptor.stride_bytes) *
				static_cast<uint64_t>(descriptor.height);
			if (static_cast<uint64_t>(descriptor.size_bytes) < min_size_bytes) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} size={} smaller than minimum {} "
					"for stride*height",
					slot,
					descriptor.size_bytes,
					min_size_bytes));
			}

			if (slot == 0 && descriptor.plane_type != FramePlaneType::Left) {
				return cvmmap::unexpected("v2 descriptor slot 0 must be LEFT plane");
			}
			if (slot == 1 && descriptor.plane_type != FramePlaneType::Depth) {
				return cvmmap::unexpected(
					"v2 descriptor slot 1 must be DEPTH plane when active");
			}
			if (slot == 2 && descriptor.plane_type != FramePlaneType::Confidence) {
				return cvmmap::unexpected(
					"v2 descriptor slot 2 must be CONFIDENCE plane when active");
			}
			if (slot == 3 &&
				descriptor.plane_type != FramePlaneType::EncodedAccessUnit) {
				return cvmmap::unexpected(
					"v2 descriptor slot 3 must be ENCODED_ACCESS_UNIT plane when active");
			}
			if (slot >= 4) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} active but unsupported in current rollout "
					"(only slots 0:LEFT, 1:DEPTH, 2:CONFIDENCE, 3:ENCODED_ACCESS_UNIT "
					"are allowed)",
					slot));
			}
			if (slot == 1 && (descriptor.pixel_format != PixelFormat::GRAY ||
							  descriptor.depth != Depth::F32)) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 depth descriptor must be GRAY/F32, got "
					"pixel_format={} depth={}",
					static_cast<uint8_t>(descriptor.pixel_format),
					static_cast<uint8_t>(descriptor.depth)));
			}
			if (slot == 2 && (descriptor.pixel_format != PixelFormat::GRAY ||
							  descriptor.depth != Depth::F32)) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 confidence descriptor must be GRAY/F32, got "
					"pixel_format={} depth={}",
					static_cast<uint8_t>(descriptor.pixel_format),
					static_cast<uint8_t>(descriptor.depth)));
			}
			if (slot == 3 && (descriptor.pixel_format != PixelFormat::GRAY ||
							  descriptor.depth != Depth::U8)) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 encoded AU descriptor must be GRAY/U8, got "
					"pixel_format={} depth={}",
					static_cast<uint8_t>(descriptor.pixel_format),
					static_cast<uint8_t>(descriptor.depth)));
			}
			if (slot == 0 && descriptor.offset_bytes != 0) {
				return cvmmap::unexpected(
					"v2 descriptor slot 0 must start at payload offset 0");
			}
			if (descriptor.offset_bytes != expected_next_offset) {
				return cvmmap::unexpected(cvmmap::format(
					"v2 descriptor slot {} offset={} expected contiguous offset {}",
					slot,
					descriptor.offset_bytes,
					expected_next_offset));
			}
			expected_next_offset = descriptor.offset_bytes + descriptor.size_bytes;
		} else if (!is_empty) {
			return cvmmap::unexpected(cvmmap::format(
				"v2 descriptor slot {} must be empty when inactive",
				slot));
		}
	}

	if (header.versions_minor >= FRAME_METADATA_V2_MINOR_ENCODED_AU &&
		((header.plane_presence_mask & 0x08u) != 0)) {
		if (encoded_extension.encoded_codec == EncodedCodec::Unknown) {
			return cvmmap::unexpected(
				"v2.1 encoded plane requires a non-unknown encoded_codec");
		}
		if (encoded_extension.encoded_bitstream_format ==
			EncodedBitstreamFormat::Unknown) {
			return cvmmap::unexpected(
				"v2.1 encoded plane requires a non-unknown encoded_bitstream_format");
		}
	}

	return {};
}

} // namespace cvmmap::protocol
