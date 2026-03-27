#include <cvmmap/parser.hpp>

#include <algorithm>
#include <cstring>
#include <cvmmap/compat/format.hpp>
#include <limits>
#include <cvmmap/compat/expected.hpp>

namespace cvmmap {

namespace {
constexpr uint16_t V2_DESCRIPTORS_OFFSET = 64;
constexpr uint16_t V2_DESCRIPTOR_SIZE = 24;
constexpr uint16_t V2_DESCRIPTOR_CAPACITY = 4;
constexpr uint8_t V2_MAX_PLANE_COUNT = 4;
constexpr uint8_t V2_VALID_PLANE_MASK = 0x0F;
constexpr size_t CV_MMAP_MAGIC_LEN = frame_metadata_t::CV_MMAP_MAGIC.size();

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

bool is_empty_descriptor(const frame_plane_descriptor_v2_t &desc) {
  return desc.width == 0 && desc.height == 0 && desc.stride_bytes == 0 &&
         desc.offset_bytes == 0 && desc.size_bytes == 0;
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

uint8_t popcount_u8(const uint8_t value) {
  uint8_t count = 0;
  for (uint8_t bits = value; bits != 0; bits >>= 1) {
    count += static_cast<uint8_t>(bits & 0x01u);
  }
  return count;
}

frame_metadata_v2_encoded_extension_t encoded_extension_from_header(
    const frame_metadata_v2_header_t &header) {
  frame_metadata_v2_encoded_extension_t ext{};
  std::memcpy(&ext, header.reserved_0, sizeof(ext));
  return ext;
}

constexpr bool is_supported_body_coordinate_system(
    const BodyCoordinateSystem value) {
  switch (value) {
  case BodyCoordinateSystem::Unknown:
  case BodyCoordinateSystem::Image:
  case BodyCoordinateSystem::RightHandedYUp:
    return true;
  default:
    return false;
  }
}

constexpr bool is_supported_body_reference_frame(
    const BodyReferenceFrame value) {
  switch (value) {
  case BodyReferenceFrame::Unknown:
  case BodyReferenceFrame::Camera:
  case BodyReferenceFrame::World:
    return true;
  default:
    return false;
  }
}

cvmmap::expected<frame_info_t, std::string>
frame_info_from_v2_descriptor(const frame_plane_descriptor_v2_t &desc) {
  if (desc.width > std::numeric_limits<uint16_t>::max()) {
    return cvmmap::unexpected(
        cvmmap::format("plane width {} exceeds frame_info_t limit", desc.width));
  }
  if (desc.height > std::numeric_limits<uint16_t>::max()) {
    return cvmmap::unexpected(
        cvmmap::format("plane height {} exceeds frame_info_t limit", desc.height));
  }
  const auto channels = channels_from_pixel_format(desc.pixel_format);
  if (channels == 0) {
    return cvmmap::unexpected(
        cvmmap::format("unsupported pixel_format={} in v2 descriptor",
                    static_cast<uint8_t>(desc.pixel_format)));
  }

  frame_info_t info{};
  info.width = static_cast<uint16_t>(desc.width);
  info.height = static_cast<uint16_t>(desc.height);
  info.channels = channels;
  info.depth = desc.depth;
  info.pixel_format = desc.pixel_format;
  info.buffer_size = desc.size_bytes;
  return info;
}
} // namespace

cvmmap::expected<parsed_frame_metadata_t, std::string>
parse_frame_metadata_regions(std::span<const uint8_t> metadata_region,
                             std::span<const uint8_t> payload_region) {
  if (metadata_region.size() < SHM_PAYLOAD_OFFSET) {
    return cvmmap::unexpected(cvmmap::format("metadata region too small: {} < {}",
                                       metadata_region.size(),
                                       SHM_PAYLOAD_OFFSET));
  }

  frame_metadata_t metadata{};
  std::memcpy(&metadata, metadata_region.data(), sizeof(frame_metadata_t));
  if (!std::equal(metadata.magic, metadata.magic + CV_MMAP_MAGIC_LEN,
                  frame_metadata_t::CV_MMAP_MAGIC.data())) {
    return cvmmap::unexpected("invalid magic");
  }

  if (metadata.versions_major == 0 && metadata.versions_minor == 0 ||
      metadata.versions_major == FRAME_METADATA_V1_MAJOR) {
    if (metadata.info.width == 0 || metadata.info.height == 0) {
      return cvmmap::unexpected("v1 frame dimensions must be non-zero");
    }
    if (metadata.info.channels == 0) {
      return cvmmap::unexpected("v1 channels must be non-zero");
    }
    if (metadata.info.buffer_size == 0) {
      return cvmmap::unexpected("v1 buffer_size must be non-zero");
    }

    const uint64_t min_row_bytes =
        static_cast<uint64_t>(metadata.info.width) *
        static_cast<uint64_t>(metadata.info.pixelSize());
    const uint64_t min_total_bytes =
        min_row_bytes * static_cast<uint64_t>(metadata.info.height);
    if (min_total_bytes == 0 || metadata.info.buffer_size < min_total_bytes) {
      return cvmmap::unexpected(
          cvmmap::format("v1 buffer_size={} smaller than minimum {}",
                      metadata.info.buffer_size, min_total_bytes));
    }
    if (metadata.info.buffer_size > payload_region.size()) {
      return cvmmap::unexpected(
          cvmmap::format("v1 buffer_size={} exceeds shared payload capacity {}",
                      metadata.info.buffer_size, payload_region.size()));
    }

    parsed_frame_metadata_t out{};
    out.normalized_metadata = metadata;
    out.left_plane = std::span<const uint8_t>(payload_region.data(),
                                              metadata.info.buffer_size);
    out.depth_unit = DepthUnit::Unknown;
    out.depth_info.reset();
    out.depth_plane = {};
    out.confidence_info.reset();
    out.confidence_plane = {};
    return out;
  }

  if (metadata.versions_major != FRAME_METADATA_V2_MAJOR) {
    return cvmmap::unexpected(cvmmap::format(
        "incompatible metadata major version {}; supported: {} or {}",
        metadata.versions_major, FRAME_METADATA_V1_MAJOR,
        FRAME_METADATA_V2_MAJOR));
  }

  frame_metadata_v2_t metadata_v2{};
  std::memcpy(&metadata_v2, metadata_region.data(),
              sizeof(frame_metadata_v2_t));
  const auto &header = metadata_v2.header;

  if (header.plane_count < 1 || header.plane_count > V2_MAX_PLANE_COUNT) {
    return cvmmap::unexpected(
        cvmmap::format("v2 plane_count={} out of bounds [1, {}]",
                    header.plane_count, V2_MAX_PLANE_COUNT));
  }
  if ((header.plane_presence_mask & ~V2_VALID_PLANE_MASK) != 0) {
    return cvmmap::unexpected(
        cvmmap::format("v2 plane_presence_mask=0x{:02x} has invalid upper bits",
                    header.plane_presence_mask));
  }
  if (header.versions_minor == FRAME_METADATA_V2_MINOR_BASE) {
    const auto expected_mask =
        static_cast<uint8_t>((1u << header.plane_count) - 1u);
    if (header.plane_presence_mask != expected_mask) {
      return cvmmap::unexpected(
          cvmmap::format("v2 plane_presence_mask=0x{:02x} does not match expected "
                      "contiguous mask 0x{:02x}",
                      header.plane_presence_mask, expected_mask));
    }
  } else {
    if ((header.plane_presence_mask & 0x01u) == 0) {
      return cvmmap::unexpected(
          "v2.1 plane_presence_mask must include slot 0 LEFT plane");
    }
    const auto expected_plane_count = popcount_u8(header.plane_presence_mask);
    if (header.plane_count != expected_plane_count) {
      return cvmmap::unexpected(
          cvmmap::format("v2.1 plane_count={} does not match popcount(mask)={}",
                      header.plane_count, expected_plane_count));
    }
  }
  if (header.plane_descriptors_offset != V2_DESCRIPTORS_OFFSET) {
    return cvmmap::unexpected(
        cvmmap::format("v2 plane_descriptors_offset={} expected {}",
                    header.plane_descriptors_offset, V2_DESCRIPTORS_OFFSET));
  }
  if (header.plane_descriptor_size != V2_DESCRIPTOR_SIZE) {
    return cvmmap::unexpected(
        cvmmap::format("v2 plane_descriptor_size={} expected {}",
                    header.plane_descriptor_size, V2_DESCRIPTOR_SIZE));
  }
  if (header.plane_descriptor_capacity != V2_DESCRIPTOR_CAPACITY) {
    return cvmmap::unexpected(
        cvmmap::format("v2 plane_descriptor_capacity={} expected {}",
                    header.plane_descriptor_capacity, V2_DESCRIPTOR_CAPACITY));
  }
  if (header.payload_size_bytes == 0) {
    return cvmmap::unexpected("v2 payload_size_bytes must be non-zero");
  }
  if (!is_supported_depth_unit(header.depth_unit)) {
    return cvmmap::unexpected(
        cvmmap::format("v2 depth_unit={} is unsupported",
                    static_cast<uint8_t>(header.depth_unit)));
  }
  if (header.payload_size_bytes > payload_region.size()) {
    return cvmmap::unexpected(cvmmap::format(
        "v2 payload_size_bytes={} exceeds shared payload capacity {}",
        header.payload_size_bytes, payload_region.size()));
  }

  const auto encoded_ext = encoded_extension_from_header(header);
  if (header.versions_minor >= FRAME_METADATA_V2_MINOR_ENCODED_AU) {
    if (!is_supported_encoded_codec(encoded_ext.encoded_codec)) {
      return cvmmap::unexpected(
          cvmmap::format("v2.1 encoded_codec={} is unsupported",
                         static_cast<uint8_t>(encoded_ext.encoded_codec)));
    }
    if (!is_supported_encoded_bitstream_format(
            encoded_ext.encoded_bitstream_format)) {
      return cvmmap::unexpected(cvmmap::format(
          "v2.1 encoded_bitstream_format={} is unsupported",
          static_cast<uint8_t>(encoded_ext.encoded_bitstream_format)));
    }
  }

  uint32_t expected_next_offset = 0;
  for (size_t slot = 0; slot < metadata_v2.descriptors.size(); ++slot) {
    const auto &desc = metadata_v2.descriptors[slot];
    const bool is_active =
        header.versions_minor == FRAME_METADATA_V2_MINOR_BASE
            ? slot < header.plane_count
            : ((header.plane_presence_mask & (1u << slot)) != 0);
    const bool is_empty = is_empty_descriptor(desc);

    if (is_active) {
      if (is_empty) {
        return cvmmap::unexpected(
            cvmmap::format("v2 descriptor slot {} is active but empty", slot));
      }
      if (desc.width == 0 || desc.height == 0 || desc.stride_bytes == 0 ||
          desc.size_bytes == 0) {
        return cvmmap::unexpected(cvmmap::format(
            "v2 descriptor slot {} has zero geometric/size field", slot));
      }
      if (desc.offset_bytes > header.payload_size_bytes) {
        return cvmmap::unexpected(cvmmap::format(
            "v2 descriptor slot {} offset={} exceeds payload_size_bytes={}",
            slot, desc.offset_bytes, header.payload_size_bytes));
      }
      if (desc.size_bytes > (header.payload_size_bytes - desc.offset_bytes)) {
        return cvmmap::unexpected(
            cvmmap::format("v2 descriptor slot {} size={} out of bounds for "
                        "payload_size_bytes={} and offset={}",
                        slot, desc.size_bytes, header.payload_size_bytes,
                        desc.offset_bytes));
      }

      const auto channels = channels_from_pixel_format(desc.pixel_format);
      if (channels == 0) {
        return cvmmap::unexpected(
            cvmmap::format("v2 descriptor slot {} has unsupported pixel_format={}",
                        slot, static_cast<uint8_t>(desc.pixel_format)));
      }

      const auto bytes_per_channel = bytes_per_channel_from_depth(desc.depth);
      if (bytes_per_channel == 0) {
        return cvmmap::unexpected(
            cvmmap::format("v2 descriptor slot {} has unsupported depth={}", slot,
                        static_cast<uint8_t>(desc.depth)));
      }

      const uint64_t min_stride_bytes =
          static_cast<uint64_t>(desc.width) * static_cast<uint64_t>(channels) *
          static_cast<uint64_t>(bytes_per_channel);
      if (static_cast<uint64_t>(desc.stride_bytes) < min_stride_bytes) {
        return cvmmap::unexpected(
            cvmmap::format("v2 descriptor slot {} stride={} smaller than minimum "
                        "{} for width/channels/depth",
                        slot, desc.stride_bytes, min_stride_bytes));
      }

      const uint64_t min_size_bytes = static_cast<uint64_t>(desc.stride_bytes) *
                                      static_cast<uint64_t>(desc.height);
      if (static_cast<uint64_t>(desc.size_bytes) < min_size_bytes) {
        return cvmmap::unexpected(
            cvmmap::format("v2 descriptor slot {} size={} smaller than minimum {} "
                        "for stride*height",
                        slot, desc.size_bytes, min_size_bytes));
      }

      if (slot == 0 && desc.plane_type != FramePlaneType::Left) {
        return cvmmap::unexpected("v2 descriptor slot 0 must be LEFT plane");
      }
      if (slot == 1 && desc.plane_type != FramePlaneType::Depth) {
        return cvmmap::unexpected(
            "v2 descriptor slot 1 must be DEPTH plane when active");
      }
      if (slot == 2 && desc.plane_type != FramePlaneType::Confidence) {
        return cvmmap::unexpected(
            "v2 descriptor slot 2 must be CONFIDENCE plane when active");
      }
      if (slot == 3 && desc.plane_type != FramePlaneType::EncodedAccessUnit) {
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
      if (slot == 1 && (desc.pixel_format != PixelFormat::GRAY ||
                        desc.depth != Depth::F32)) {
        return cvmmap::unexpected(
            cvmmap::format("v2 depth descriptor must be GRAY/F32, got "
                        "pixel_format={} depth={}",
                        static_cast<uint8_t>(desc.pixel_format),
                        static_cast<uint8_t>(desc.depth)));
      }
      if (slot == 2 && (desc.pixel_format != PixelFormat::GRAY ||
                        desc.depth != Depth::F32)) {
        return cvmmap::unexpected(
            cvmmap::format("v2 confidence descriptor must be GRAY/F32, got "
                        "pixel_format={} depth={}",
                        static_cast<uint8_t>(desc.pixel_format),
                        static_cast<uint8_t>(desc.depth)));
      }
      if (slot == 3 && (desc.pixel_format != PixelFormat::GRAY ||
                        desc.depth != Depth::U8)) {
        return cvmmap::unexpected(
            cvmmap::format("v2 encoded AU descriptor must be GRAY/U8, got "
                        "pixel_format={} depth={}",
                        static_cast<uint8_t>(desc.pixel_format),
                        static_cast<uint8_t>(desc.depth)));
      }
      if (slot == 0 && desc.offset_bytes != 0) {
        return cvmmap::unexpected(
            "v2 descriptor slot 0 must start at payload offset 0");
      }
      if (desc.offset_bytes != expected_next_offset) {
        return cvmmap::unexpected(cvmmap::format(
            "v2 descriptor slot {} offset={} expected contiguous offset {}",
            slot, desc.offset_bytes, expected_next_offset));
      }
      expected_next_offset = desc.offset_bytes + desc.size_bytes;
    } else if (!is_empty) {
      return cvmmap::unexpected(cvmmap::format(
          "v2 descriptor slot {} must be empty when inactive", slot));
    }
  }

  const auto &left_desc = metadata_v2.descriptors[0];
  auto left_info_res = frame_info_from_v2_descriptor(left_desc);
  if (!left_info_res) {
    return cvmmap::unexpected(
        cvmmap::format("v2 left descriptor invalid: {}", left_info_res.error()));
  }

  parsed_frame_metadata_t out{};
  out.normalized_metadata = {};
  std::copy(std::begin(header.magic), std::end(header.magic),
            out.normalized_metadata.magic);
  out.normalized_metadata.versions_major = header.versions_major;
  out.normalized_metadata.versions_minor = header.versions_minor;
  out.normalized_metadata.frame_count = header.frame_id;
  out.normalized_metadata.timestamp_ns = header.capture_ts_ns;
  out.normalized_metadata.info = *left_info_res;
  out.depth_unit = header.depth_unit;

  out.left_plane = std::span<const uint8_t>(
      payload_region.data() + left_desc.offset_bytes, left_desc.size_bytes);
  out.depth_info.reset();
  out.depth_plane = {};
  out.confidence_info.reset();
  out.confidence_plane = {};
  out.encoded_codec = EncodedCodec::Unknown;
  out.encoded_bitstream_format = EncodedBitstreamFormat::Unknown;
  out.encoded_flags = 0;
  out.encoded_frame_rate_num = 0;
  out.encoded_frame_rate_den = 0;
  out.encoded_stream_pts_ns = 0;
  out.encoded_access_unit = {};

  const auto has_depth =
      header.versions_minor == FRAME_METADATA_V2_MINOR_BASE
          ? header.plane_count >= 2
          : ((header.plane_presence_mask & 0x02u) != 0);
  if (has_depth) {
    const auto &depth_desc = metadata_v2.descriptors[1];
    auto depth_info_res = frame_info_from_v2_descriptor(depth_desc);
    if (!depth_info_res) {
      return cvmmap::unexpected(cvmmap::format("v2 depth descriptor invalid: {}",
                                         depth_info_res.error()));
    }
    out.depth_info = *depth_info_res;
    out.depth_plane = std::span<const uint8_t>(
        payload_region.data() + depth_desc.offset_bytes, depth_desc.size_bytes);
  }

  const auto has_confidence =
      header.versions_minor == FRAME_METADATA_V2_MINOR_BASE
          ? header.plane_count >= 3
          : ((header.plane_presence_mask & 0x04u) != 0);
  if (has_confidence) {
    const auto &confidence_desc = metadata_v2.descriptors[2];
    auto confidence_info_res = frame_info_from_v2_descriptor(confidence_desc);
    if (!confidence_info_res) {
      return cvmmap::unexpected(cvmmap::format("v2 confidence descriptor invalid: {}",
                                         confidence_info_res.error()));
    }
    out.confidence_info = *confidence_info_res;
    out.confidence_plane = std::span<const uint8_t>(
        payload_region.data() + confidence_desc.offset_bytes,
        confidence_desc.size_bytes);
  }

  const auto has_encoded_access_unit =
      header.versions_minor >= FRAME_METADATA_V2_MINOR_ENCODED_AU &&
      ((header.plane_presence_mask & 0x08u) != 0);
  if (has_encoded_access_unit) {
    const auto &encoded_desc = metadata_v2.descriptors[3];
    out.encoded_codec = encoded_ext.encoded_codec;
    out.encoded_bitstream_format = encoded_ext.encoded_bitstream_format;
    out.encoded_flags = encoded_ext.encoded_flags;
    out.encoded_frame_rate_num = encoded_ext.encoded_frame_rate_num;
    out.encoded_frame_rate_den = encoded_ext.encoded_frame_rate_den;
    out.encoded_stream_pts_ns = encoded_ext.encoded_stream_pts_ns;
    out.encoded_access_unit = std::span<const uint8_t>(
        payload_region.data() + encoded_desc.offset_bytes, encoded_desc.size_bytes);
    if (out.encoded_codec == EncodedCodec::Unknown) {
      return cvmmap::unexpected(
          "v2.1 encoded plane requires a non-unknown encoded_codec");
    }
    if (out.encoded_bitstream_format == EncodedBitstreamFormat::Unknown) {
      return cvmmap::unexpected(
          "v2.1 encoded plane requires a non-unknown encoded_bitstream_format");
    }
  }

  return out;
}

cvmmap::expected<body_tracking_frame_t, std::string>
parse_body_tracking_message(std::span<const uint8_t> message) {
  if (message.size() < sizeof(body_tracking_message_header_t)) {
    return cvmmap::unexpected(cvmmap::format(
        "body message too small: {} < {}", message.size(),
        sizeof(body_tracking_message_header_t)));
  }

  body_tracking_message_header_t header{};
  std::memcpy(&header, message.data(), sizeof(header));

  if (header._magic != BODY_TRACKING_MAGIC) {
    return cvmmap::unexpected(cvmmap::format(
        "invalid body message magic: expected 0x{:02x}, got 0x{:02x}",
        BODY_TRACKING_MAGIC, header._magic));
  }
  if (header.versions_major != VERSION_MAJOR) {
    return cvmmap::unexpected(cvmmap::format(
        "unsupported body message major version: expected {}, got {}",
        VERSION_MAJOR, header.versions_major));
  }
  if (header.body_record_size != sizeof(body_tracking_body_t)) {
    return cvmmap::unexpected(cvmmap::format(
        "invalid body_record_size: expected {}, got {}",
        sizeof(body_tracking_body_t), header.body_record_size));
  }

  const auto expected_payload_size =
      static_cast<size_t>(header.body_count) * sizeof(body_tracking_body_t);
  if (expected_payload_size != header.payload_size_bytes) {
    return cvmmap::unexpected(cvmmap::format(
        "invalid payload_size_bytes: expected {}, got {}", expected_payload_size,
        header.payload_size_bytes));
  }

  const auto total_size = sizeof(body_tracking_message_header_t) +
                          static_cast<size_t>(header.payload_size_bytes);
  if (message.size() < total_size) {
    return cvmmap::unexpected(cvmmap::format(
        "body message truncated: {} < {}", message.size(), total_size));
  }

  auto body_format_value = static_cast<uint8_t>(header.body_format);
  if (body_format_value > static_cast<uint8_t>(BodyFormat::Body38)) {
    return cvmmap::unexpected(
        cvmmap::format("unsupported body_format={}", body_format_value));
  }
  auto body_selection_value = static_cast<uint8_t>(header.body_selection);
  if (body_selection_value >
      static_cast<uint8_t>(BodyKeypointSelection::UpperBody)) {
    return cvmmap::unexpected(
        cvmmap::format("unsupported body_selection={}", body_selection_value));
  }
  auto detection_model_value = static_cast<uint8_t>(header.detection_model);
  if (detection_model_value >
      static_cast<uint8_t>(BodyTrackingModel::HumanBodyAccurate)) {
    return cvmmap::unexpected(
        cvmmap::format("unsupported detection_model={}", detection_model_value));
  }
  auto precision_value = static_cast<uint8_t>(header.inference_precision);
  if (precision_value > static_cast<uint8_t>(InferencePrecision::INT8)) {
    return cvmmap::unexpected(
        cvmmap::format("unsupported inference_precision={}", precision_value));
  }
  if (!is_supported_body_coordinate_system(header.coordinate_system())) {
    return cvmmap::unexpected(cvmmap::format(
        "unsupported coordinate_system={}",
        static_cast<uint8_t>(header.coordinate_system())));
  }
  if (!is_supported_body_reference_frame(header.reference_frame())) {
    return cvmmap::unexpected(cvmmap::format(
        "unsupported reference_frame={}",
        static_cast<uint8_t>(header.reference_frame())));
  }

  body_tracking_frame_t out{};
  out.header = header;
  out.bodies.resize(header.body_count);
  if (header.body_count > 0) {
    std::memcpy(out.bodies.data(),
                message.data() + sizeof(body_tracking_message_header_t),
                expected_payload_size);
  }
  return out;
}

} // namespace cvmmap
