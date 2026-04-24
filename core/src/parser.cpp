#include <cvmmap/parser.hpp>
#include <cvmmap/protocol.hpp>

#include <algorithm>
#include <cstring>
#include <cvmmap/compat/format.hpp>
#include <cvmmap/compat/expected.hpp>

namespace cvmmap {

namespace {
constexpr size_t CV_MMAP_MAGIC_LEN = frame_metadata_t::CV_MMAP_MAGIC.size();

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

  auto validation =
      protocol::validate_frame_metadata_v2(metadata_v2, payload_region.size());
  if (!validation) {
    return cvmmap::unexpected(validation.error());
  }
  const auto encoded_ext = protocol::encoded_extension_from_header(header);

  const auto &left_desc = metadata_v2.descriptors[0];
  auto left_info_res = protocol::frame_info_from_v2_descriptor(left_desc);
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
      protocol::is_frame_metadata_v2_slot_active(header, 1);
  if (has_depth) {
    const auto &depth_desc = metadata_v2.descriptors[1];
    auto depth_info_res = protocol::frame_info_from_v2_descriptor(depth_desc);
    if (!depth_info_res) {
      return cvmmap::unexpected(cvmmap::format("v2 depth descriptor invalid: {}",
                                         depth_info_res.error()));
    }
    out.depth_info = *depth_info_res;
    out.depth_plane = std::span<const uint8_t>(
        payload_region.data() + depth_desc.offset_bytes, depth_desc.size_bytes);
  }

  const auto has_confidence =
      protocol::is_frame_metadata_v2_slot_active(header, 2);
  if (has_confidence) {
    const auto &confidence_desc = metadata_v2.descriptors[2];
    auto confidence_info_res = protocol::frame_info_from_v2_descriptor(confidence_desc);
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
      protocol::is_frame_metadata_v2_slot_active(header, 3);
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
