#include "frame_publisher.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace app {
namespace {

struct encoded_plane_view_payload_t {
	cvmmap::EncodedCodec codec{cvmmap::EncodedCodec::Unknown};
	cvmmap::EncodedBitstreamFormat bitstream_format{
		cvmmap::EncodedBitstreamFormat::Unknown};
	uint16_t flags{0};
	uint16_t frame_rate_num{0};
	uint16_t frame_rate_den{0};
	uint64_t stream_pts_ns{0};
	std::span<const uint8_t> bytes{};
};

} // namespace

FramePublisher::shm_state_t::shm_state_t(std::string name, int shm_fd)
	: name(std::move(name)), shm_fd(shm_fd) {}

FramePublisher::shm_state_t::~shm_state_t() {
	if (shm_fd != -1) {
		spdlog::debug("closing shared memory `{}` (fd={})", name, shm_fd);
		close(shm_fd);
		shm_unlink(name.c_str());
	}
}

FramePublisher::shm_state_t::shm_state_t(shm_state_t &&other) noexcept
	: name(std::move(other.name)), shm_fd(other.shm_fd) {
	other.shm_fd = -1;
}

FramePublisher::shm_state_t &FramePublisher::shm_state_t::operator=(shm_state_t &&other) noexcept {
	if (this != &other) {
		if (shm_fd != -1) {
			close(shm_fd);
			shm_unlink(name.c_str());
		}
		name = std::move(other.name);
		shm_fd = other.shm_fd;
		other.shm_fd = -1;
	}
	return *this;
}

cvmmap::expected<FramePublisher::shm_state_t, int>
FramePublisher::shm_state_t::open(const std::string &name) {
	spdlog::debug("opening shared memory `{}`", name);
	int shm_fd = shm_open(
		name.c_str(),
		O_CREAT | O_RDWR,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	if (shm_fd == -1) {
		if (errno == EACCES || errno == EEXIST) {
			auto err = shm_unlink(name.c_str());
			if (err == -1) {
				spdlog::error(
					"unlinking shared memory `{}`: {}",
					name,
					strerror(errno));
				return cvmmap::unexpected(errno);
			}
			spdlog::warn("unlinked shared memory `{}`", name);
			return open(name);
		}
		return cvmmap::unexpected(errno);
	}
	spdlog::debug("opened shared memory `{}` (fd={})", name, shm_fd);
	return shm_state_t(name, shm_fd);
}

FramePublisher::frame_state_t::frame_state_t(std::span<uint8_t> buf)
	: mmap_ptr(buf.data()),
  metadata_buffer(buf.subspan(0, SHM_PAYLOAD_OFFSET)),
  image_buffer(
		  buf.subspan(SHM_PAYLOAD_OFFSET, buf.size() - SHM_PAYLOAD_OFFSET)) {
	assert(total_buffer_size() == buf.size());
	std::fill(metadata_buffer.begin(), metadata_buffer.end(), 0);
	metadata().header.ensure_magic();
}

FramePublisher::frame_state_t::~frame_state_t() {
	if (mmap_ptr) {
		spdlog::debug("closing frame state (mmap_ptr={})", static_cast<void *>(mmap_ptr));
		munmap(mmap_ptr, total_buffer_size());
	}
}

FramePublisher::frame_state_t::frame_state_t(frame_state_t &&other) noexcept
	: mmap_ptr(other.mmap_ptr),
  metadata_buffer(other.metadata_buffer),
  image_buffer(other.image_buffer) {
	other.mmap_ptr = {};
	other.metadata_buffer = {};
	other.image_buffer = {};
}

FramePublisher::frame_state_t &FramePublisher::frame_state_t::operator=(frame_state_t &&other) noexcept {
	if (this != &other) {
		if (mmap_ptr) {
			munmap(mmap_ptr, total_buffer_size());
		}
		mmap_ptr = other.mmap_ptr;
		metadata_buffer = other.metadata_buffer;
		image_buffer = other.image_buffer;
		other.mmap_ptr = {};
		other.metadata_buffer = {};
		other.image_buffer = {};
	}
	return *this;
}

cvmmap::expected<FramePublisher::frame_state_t, int>
FramePublisher::frame_state_t::open(int shm_fd, size_t size) {
	if (ftruncate(shm_fd, size) == -1) {
		spdlog::error(
			"truncate shared memory: fd={}, errno={} ({})",
			shm_fd,
			errno,
			strerror(errno));
		return cvmmap::unexpected(errno);
	}
	auto *ptr = static_cast<uint8_t *>(
		mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
	if (ptr == MAP_FAILED) {
		spdlog::error(
			"mmap shared memory: fd={}, errno={} ({})",
			shm_fd,
			errno,
			strerror(errno));
		return cvmmap::unexpected(errno);
	}
	return frame_state_t(std::span<uint8_t>(ptr, size));
}

size_t FramePublisher::frame_state_t::total_buffer_size() const {
	return metadata_buffer.size() + image_buffer.size();
}

frame_metadata_v2_t &FramePublisher::frame_state_t::metadata() {
	return *reinterpret_cast<frame_metadata_v2_t *>(metadata_buffer.data());
}

void FramePublisher::frame_state_t::write_metadata(
	const frame_metadata_v2_t &metadata_value) {
	std::memcpy(metadata_buffer.data(), &metadata_value, sizeof(metadata_value));
}

cvmmap::expected<FramePublisher, std::string>
FramePublisher::Create(const Config &config, zmq::socket_t &sync_socket) {
	auto shm_state = shm_state_t::open(config.shm_name());
	if (!shm_state) {
		return cvmmap::unexpected(cvmmap::format(
			"opening shared memory `{}`: {}",
			config.shm_name(),
			shm_state.error()));
	}

	auto undistort_pass = preprocess::make_undistort_pass(config.preprocess);
	if (undistort_pass) {
		if (config.video.backend == BackendType::ZED) {
			spdlog::warn(
				"ignoring preprocess.undistort for direct-frame backend; direct-fill path publishes native frames without producer-side undistort");
			undistort_pass.reset();
		} else {
			spdlog::info("undistort preprocess pass is enabled");
		}
	}

	spdlog::debug(
		"created shared memory `{}` (fd={})",
		config.shm_name(),
		shm_state->shm_fd);
	return FramePublisher(config, sync_socket, std::move(*shm_state), std::move(undistort_pass));
}

FramePublisher::FramePublisher(
	const Config &config,
	zmq::socket_t &sync_socket,
	shm_state_t shm_state,
	std::optional<preprocess::UndistortPass> undistort_pass)
	: config_(&config),
	  sync_socket_(&sync_socket),
	  shm_state_(std::move(shm_state)),
	  undistort_pass_(std::move(undistort_pass)) {}

FramePublisher::FramePublisher(FramePublisher &&other) noexcept
	: config_(other.config_),
	  sync_socket_(other.sync_socket_),
	  shm_state_(std::move(other.shm_state_)),
	  frame_state_(std::move(other.frame_state_)),
	  sync_msg_(std::move(other.sync_msg_)),
	  pending_encoded_by_timestamp_(std::move(other.pending_encoded_by_timestamp_)),
	  undistort_pass_(std::move(other.undistort_pass_)) {}

FramePublisher &FramePublisher::operator=(FramePublisher &&other) noexcept {
	if (this != &other) {
		config_ = other.config_;
		sync_socket_ = other.sync_socket_;
		shm_state_ = std::move(other.shm_state_);
		frame_state_ = std::move(other.frame_state_);
		sync_msg_ = std::move(other.sync_msg_);
		pending_encoded_by_timestamp_ = std::move(other.pending_encoded_by_timestamp_);
		undistort_pass_ = std::move(other.undistort_pass_);
	}
	return *this;
}
FramePublisher::~FramePublisher() = default;

void FramePublisher::Reset() {
	frame_state_.reset();
	sync_msg_.reset();
	std::lock_guard lock(pending_encoded_mutex_);
	pending_encoded_by_timestamp_.clear();
}

uint64_t FramePublisher::now_ns() const {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

std::optional<uint32_t> FramePublisher::to_u32(size_t value) const {
	if (value > std::numeric_limits<uint32_t>::max()) {
		return std::nullopt;
	}
	return static_cast<uint32_t>(value);
}

DepthUnit FramePublisher::determine_depth_unit() const {
	if (config_->video.backend != BackendType::ZED || !config_->zed.has_value()) {
		return DepthUnit::Unknown;
	}
	const auto &zed = *config_->zed;
	const bool body_tracking_uses_meters =
		zed.body_tracking.has_value() && zed.body_tracking->enabled;
	return body_tracking_uses_meters ? DepthUnit::Meter : DepthUnit::Millimeter;
}

std::optional<FramePublisher::encoded_plane_view_t>
FramePublisher::TakeEncodedPlane(
	uint64_t timestamp_ns,
	std::vector<uint8_t> &storage) {
	std::lock_guard lock(pending_encoded_mutex_);
	auto it = pending_encoded_by_timestamp_.find(timestamp_ns);
	if (it == pending_encoded_by_timestamp_.end()) {
		return std::nullopt;
	}
	encoded_plane_view_t plane{};
	plane.codec = it->second.codec;
	plane.bitstream_format = it->second.bitstream_format;
	plane.flags = it->second.flags;
	plane.frame_rate_num = it->second.frame_rate_num;
	plane.frame_rate_den = it->second.frame_rate_den;
	plane.stream_pts_ns = it->second.stream_pts_ns;
	storage = std::move(it->second.bytes);
	plane.bytes = std::span<const uint8_t>(storage.data(), storage.size());
	pending_encoded_by_timestamp_.erase(it);
	return plane;
}

std::optional<frame_metadata_v2_t> FramePublisher::BuildV2Metadata(
	const frame_metadata_t &source_metadata,
	size_t raw_payload_size,
	const std::optional<encoded_plane_view_t> &encoded_plane) const {
	if (raw_payload_size == 0 || source_metadata.info.width == 0 ||
		source_metadata.info.height == 0 || source_metadata.info.channels == 0) {
		return std::nullopt;
	}

	const size_t encoded_payload_size = encoded_plane ? encoded_plane->bytes.size() : 0;
	auto payload_size_u32 = to_u32(raw_payload_size + encoded_payload_size);
	if (!payload_size_u32) {
		return std::nullopt;
	}

	auto make_stride = [this](size_t plane_size, uint32_t height, size_t expected_min_stride)
		-> std::optional<uint32_t> {
		if (height == 0) {
			return std::nullopt;
		}
		size_t stride = expected_min_stride;
		if (plane_size % height == 0) {
			stride = std::max(expected_min_stride, plane_size / height);
		}
		return to_u32(stride);
	};

	frame_metadata_v2_t metadata_v2{};
	std::memset(&metadata_v2, 0, sizeof(metadata_v2));
	metadata_v2.header.ensure_magic();
	metadata_v2.header.versions_major = frame_metadata_v2_header_t::VERSION_MAJOR_V2;
	metadata_v2.header.versions_minor = VERSION_MINOR;
	metadata_v2.header.frame_id = source_metadata.frame_count;
	metadata_v2.header.capture_ts_ns = source_metadata.timestamp_ns;
	metadata_v2.header.publish_seq = source_metadata.frame_count;
	metadata_v2.header.plane_count = 1;
	metadata_v2.header.plane_presence_mask = 0x01;
	metadata_v2.header.plane_descriptors_offset =
		frame_metadata_v2_header_t::PLANE_DESCRIPTORS_OFFSET;
	metadata_v2.header.plane_descriptor_size =
		frame_metadata_v2_header_t::PLANE_DESCRIPTOR_SIZE;
	metadata_v2.header.plane_descriptor_capacity =
		frame_metadata_v2_header_t::PLANE_DESCRIPTOR_CAPACITY;
	metadata_v2.header.payload_size_bytes = *payload_size_u32;
	metadata_v2.header.depth_unit = DepthUnit::Unknown;

	const size_t left_expected_stride =
		static_cast<size_t>(source_metadata.info.width) *
		static_cast<size_t>(source_metadata.info.channels) *
		static_cast<size_t>(size_of(source_metadata.info.depth));
	const size_t left_compact_size =
		left_expected_stride * static_cast<size_t>(source_metadata.info.height);

	size_t left_size = raw_payload_size;
	size_t depth_size = 0;
	size_t confidence_size = 0;
	bool depth_plane_active = false;
	bool confidence_plane_active = false;

	const size_t depth_expected_stride =
		static_cast<size_t>(source_metadata.info.width) * sizeof(float);
	const size_t depth_compact_size =
		depth_expected_stride * static_cast<size_t>(source_metadata.info.height);
	const size_t packed_extra_size =
		raw_payload_size >= left_compact_size ? raw_payload_size - left_compact_size : 0;
	const bool has_exact_depth_tail =
		left_compact_size > 0 && depth_compact_size > 0 &&
		raw_payload_size >= left_compact_size &&
		packed_extra_size == depth_compact_size;
	const bool has_exact_depth_and_confidence_tail =
		left_compact_size > 0 && depth_compact_size > 0 &&
		raw_payload_size >= left_compact_size &&
		packed_extra_size == (depth_compact_size * 2);

	if (has_exact_depth_and_confidence_tail) {
		left_size = left_compact_size;
		depth_size = depth_compact_size;
		confidence_size = depth_compact_size;
		depth_plane_active = true;
		confidence_plane_active = true;
	} else if (has_exact_depth_tail) {
		left_size = left_compact_size;
		depth_size = depth_compact_size;
		depth_plane_active = true;
	}

	auto left_size_u32 = to_u32(left_size);
	auto left_stride_u32 =
		make_stride(left_size, source_metadata.info.height, left_expected_stride);
	if (!left_size_u32 || !left_stride_u32) {
		return std::nullopt;
	}

	auto &left_descriptor = metadata_v2.plane_descriptors[0];
	left_descriptor.plane_type = FramePlaneType::LEFT;
	left_descriptor.pixel_format = source_metadata.info.pixel_format;
	left_descriptor.depth = source_metadata.info.depth;
	left_descriptor.width = source_metadata.info.width;
	left_descriptor.height = source_metadata.info.height;
	left_descriptor.stride_bytes = *left_stride_u32;
	left_descriptor.offset_bytes = 0;
	left_descriptor.size_bytes = *left_size_u32;

	if (depth_plane_active) {
		auto depth_size_u32 = to_u32(depth_size);
		auto depth_offset_u32 = to_u32(left_size);
		auto depth_stride_u32 =
			make_stride(depth_size, source_metadata.info.height, depth_expected_stride);
		if (!depth_size_u32 || !depth_offset_u32 || !depth_stride_u32) {
			return std::nullopt;
		}
		auto &depth_descriptor = metadata_v2.plane_descriptors[1];
		depth_descriptor.plane_type = FramePlaneType::DEPTH;
		depth_descriptor.pixel_format = PixelFormat::GRAY;
		depth_descriptor.depth = Depth::F32;
		depth_descriptor.width = source_metadata.info.width;
		depth_descriptor.height = source_metadata.info.height;
		depth_descriptor.stride_bytes = *depth_stride_u32;
		depth_descriptor.offset_bytes = *depth_offset_u32;
		depth_descriptor.size_bytes = *depth_size_u32;
		metadata_v2.header.plane_count = 2;
		metadata_v2.header.plane_presence_mask = 0x03;
		metadata_v2.header.depth_unit = determine_depth_unit();
	}

	if (confidence_plane_active) {
		auto confidence_size_u32 = to_u32(confidence_size);
		auto confidence_offset_u32 = to_u32(left_size + depth_size);
		auto confidence_stride_u32 = make_stride(
			confidence_size,
			source_metadata.info.height,
			depth_expected_stride);
		if (!confidence_size_u32 || !confidence_offset_u32 ||
			!confidence_stride_u32) {
			return std::nullopt;
		}
		auto &confidence_descriptor = metadata_v2.plane_descriptors[2];
		confidence_descriptor.plane_type = FramePlaneType::CONFIDENCE;
		confidence_descriptor.pixel_format = PixelFormat::GRAY;
		confidence_descriptor.depth = Depth::F32;
		confidence_descriptor.width = source_metadata.info.width;
		confidence_descriptor.height = source_metadata.info.height;
		confidence_descriptor.stride_bytes = *confidence_stride_u32;
		confidence_descriptor.offset_bytes = *confidence_offset_u32;
		confidence_descriptor.size_bytes = *confidence_size_u32;
		metadata_v2.header.plane_count = 3;
		metadata_v2.header.plane_presence_mask = 0x07;
	}

	if (encoded_plane && !encoded_plane->bytes.empty()) {
		auto encoded_offset_u32 = to_u32(left_size + depth_size + confidence_size);
		auto encoded_size_u32 = to_u32(encoded_plane->bytes.size());
		if (!encoded_offset_u32 || !encoded_size_u32) {
			return std::nullopt;
		}
		auto &encoded_descriptor = metadata_v2.plane_descriptors[3];
		encoded_descriptor.plane_type = FramePlaneType::ENCODED_ACCESS_UNIT;
		encoded_descriptor.pixel_format = PixelFormat::GRAY;
		encoded_descriptor.depth = Depth::U8;
		encoded_descriptor.width = *encoded_size_u32;
		encoded_descriptor.height = 1;
		encoded_descriptor.stride_bytes = *encoded_size_u32;
		encoded_descriptor.offset_bytes = *encoded_offset_u32;
		encoded_descriptor.size_bytes = *encoded_size_u32;
		metadata_v2.header.versions_minor =
			frame_metadata_v2_header_t::VERSION_MINOR_V2_ENCODED_AU;
		metadata_v2.header.plane_presence_mask |= 0x08;
		metadata_v2.header.plane_count = static_cast<uint8_t>(
			((metadata_v2.header.plane_presence_mask & 0x01) ? 1 : 0) +
			((metadata_v2.header.plane_presence_mask & 0x02) ? 1 : 0) +
			((metadata_v2.header.plane_presence_mask & 0x04) ? 1 : 0) + 1);

		frame_metadata_v2_encoded_extension_t encoded_extension{};
		switch (encoded_plane->codec) {
		case cvmmap::EncodedCodec::H264:
			encoded_extension.encoded_codec = EncodedCodec::H264;
			break;
		case cvmmap::EncodedCodec::H265:
			encoded_extension.encoded_codec = EncodedCodec::H265;
			break;
		case cvmmap::EncodedCodec::Unknown:
		default:
			encoded_extension.encoded_codec = EncodedCodec::UNKNOWN;
			break;
		}
		switch (encoded_plane->bitstream_format) {
		case cvmmap::EncodedBitstreamFormat::AnnexB:
			encoded_extension.encoded_bitstream_format =
				EncodedBitstreamFormat::ANNEXB;
			break;
		case cvmmap::EncodedBitstreamFormat::Unknown:
		default:
			encoded_extension.encoded_bitstream_format =
				EncodedBitstreamFormat::UNKNOWN;
			break;
		}
		encoded_extension.encoded_flags = encoded_plane->flags;
		encoded_extension.encoded_frame_rate_num = encoded_plane->frame_rate_num;
		encoded_extension.encoded_frame_rate_den = encoded_plane->frame_rate_den;
		encoded_extension.encoded_stream_pts_ns = encoded_plane->stream_pts_ns;
		std::memcpy(
			metadata_v2.header.reserved_0,
			&encoded_extension,
			sizeof(encoded_extension));
	}

	return metadata_v2;
}

void FramePublisher::EnsureMetadataState(
	const frame_metadata_t &metadata,
	size_t payload_size,
	direct_buffer_reset_hook_t before_reset) {
	if (payload_size == 0) {
		spdlog::error("metadata callback produced zero-sized picture buffer");
		return;
	}
	const auto total_buffer_size = SHM_PAYLOAD_OFFSET + payload_size;
	if (frame_state_ && before_reset) {
		before_reset(frame_state_->image_buffer);
	}
	auto fs = frame_state_t::open(shm_state_->shm_fd, total_buffer_size);
	if (!fs) {
		spdlog::error("open frame state: {}", fs.error());
		return;
	}
	auto initial_metadata = metadata;
	initial_metadata.frame_count = 0;
	initial_metadata.timestamp_ns = now_ns();
	auto initial_metadata_v2 = BuildV2Metadata(initial_metadata, payload_size);
	if (!initial_metadata_v2) {
		spdlog::error("initial ABI v2 metadata is invalid");
		return;
	}
	fs->write_metadata(*initial_metadata_v2);
	frame_state_ = std::move(*fs);
	sync_msg_.emplace(config_->name, 0);
}

void FramePublisher::OnMetadata(
	const frame_metadata_t &metadata,
	direct_buffer_reset_hook_t before_reset) {
	EnsureMetadataState(metadata, metadata.info.buffer_size, std::move(before_reset));
}

void FramePublisher::SendSyncMessage(uint32_t frame_count, uint64_t timestamp_ns) {
	try {
		std::array<uint8_t, sync_message_t::size()> buffer;
		sync_msg_->set_frame_count(frame_count);
		sync_msg_->set_timestamp_ns(timestamp_ns);
		std::copy(
			sync_msg_->as_uint8s().begin(),
			sync_msg_->as_uint8s().end(),
			buffer.begin());
		sync_socket_->send(zmq::buffer(buffer), zmq::send_flags::none);
	} catch (const zmq::error_t &e) {
		spdlog::error(
			"sending synchronization message for frame@{}: {}",
			frame_count,
			e.what());
	}
}

void FramePublisher::PublishDirectFrame(
	backends::direct_frame_t frame,
	direct_buffer_reset_hook_t before_reset) {
	if (!frame_state_ || !sync_msg_) {
		spdlog::error("direct frame callback ran before metadata initialization");
		return;
	}
	if (frame.metadata.info.buffer_size > frame_state_->image_buffer.size()) {
		if (before_reset) {
			before_reset(frame_state_->image_buffer);
		}
		const auto total_buffer_size =
			SHM_PAYLOAD_OFFSET + static_cast<size_t>(frame.metadata.info.buffer_size);
		auto resized_frame_state =
			frame_state_t::open(shm_state_->shm_fd, total_buffer_size);
		if (!resized_frame_state) {
			spdlog::error(
				"resizing shared memory for direct frame payload: {}",
				resized_frame_state.error());
			return;
		}
		frame_state_ = std::move(*resized_frame_state);
	}
	auto packed_size = frame.fill_payload(frame_state_->image_buffer);
	if (!packed_size || *packed_size == 0) {
		spdlog::error("direct frame publisher received empty payload");
		return;
	}
	frame.metadata.info.buffer_size = static_cast<uint32_t>(*packed_size);
	auto metadata_v2 = BuildV2Metadata(frame.metadata, *packed_size);
	if (!metadata_v2) {
		spdlog::error(
			"ABI v2 metadata is invalid for direct frame@{}",
			frame.metadata.frame_count);
		return;
	}
	if (metadata_v2->header.payload_size_bytes > frame_state_->image_buffer.size()) {
		spdlog::error(
			"direct frame payload ({}) exceeds shared memory payload capacity ({})",
			metadata_v2->header.payload_size_bytes,
			frame_state_->image_buffer.size());
		return;
	}
	frame_state_->write_metadata(*metadata_v2);
	SendSyncMessage(frame.metadata.frame_count, frame.metadata.timestamp_ns);
}

void FramePublisher::PublishFrame(
	std::span<uint8_t> frame_buffer,
	const frame_metadata_t &metadata) {
	if (!frame_state_ || !sync_msg_) {
		spdlog::error("frame callback ran before metadata initialization");
		return;
	}

	std::span<const uint8_t> output_buffer(frame_buffer.data(), frame_buffer.size());
	if (undistort_pass_) {
		try {
			output_buffer = undistort_pass_->apply(output_buffer, metadata.info);
		} catch (const std::exception &e) {
			spdlog::error("undistort preprocess rejected frame: {}", e.what());
			return;
		}
	}

	const auto picture_buffer_size = output_buffer.size();
	if (picture_buffer_size == 0) {
		spdlog::error("frame callback received zero-sized buffer");
		return;
	}

	std::vector<uint8_t> encoded_plane_storage{};
	auto encoded_plane = TakeEncodedPlane(metadata.timestamp_ns, encoded_plane_storage);
	const size_t total_payload_size =
		picture_buffer_size +
		(encoded_plane ? encoded_plane->bytes.size() : 0);
	if (total_payload_size > frame_state_->image_buffer.size()) {
		auto resized_frame_state = frame_state_t::open(
			shm_state_->shm_fd,
			SHM_PAYLOAD_OFFSET + total_payload_size);
		if (!resized_frame_state) {
			spdlog::error(
				"resizing shared memory for frame payload: {}",
				resized_frame_state.error());
			return;
		}
		frame_state_ = std::move(*resized_frame_state);
	}

	auto metadata_v2 = BuildV2Metadata(metadata, picture_buffer_size, encoded_plane);
	if (!metadata_v2) {
		spdlog::error("ABI v2 metadata is invalid for frame@{}", metadata.frame_count);
		return;
	}
	if (metadata_v2->header.payload_size_bytes > frame_state_->image_buffer.size()) {
		spdlog::error(
			"frame payload ({}) exceeds shared memory payload capacity ({})",
			metadata_v2->header.payload_size_bytes,
			frame_state_->image_buffer.size());
		return;
	}

	std::copy_n(
		output_buffer.begin(),
		output_buffer.size(),
		frame_state_->image_buffer.begin());
	if (encoded_plane && !encoded_plane->bytes.empty()) {
		std::copy(
			encoded_plane->bytes.begin(),
			encoded_plane->bytes.end(),
			frame_state_->image_buffer.begin() +
				static_cast<std::ptrdiff_t>(output_buffer.size()));
	}
	frame_state_->write_metadata(*metadata_v2);
	SendSyncMessage(metadata.frame_count, metadata.timestamp_ns);
}

void FramePublisher::OnEncodedAccessUnit(
	const backends::encoded_access_unit_t &access_unit) {
	std::lock_guard lock(pending_encoded_mutex_);
	pending_encoded_by_timestamp_[access_unit.source_timestamp_ns] =
		pending_encoded_access_unit_t{
			.codec = access_unit.codec,
			.bitstream_format = access_unit.bitstream_format,
			.flags = access_unit.flags,
			.frame_rate_num = access_unit.frame_rate_num,
			.frame_rate_den = access_unit.frame_rate_den,
			.source_timestamp_ns = access_unit.source_timestamp_ns,
			.stream_pts_ns = access_unit.stream_pts_ns,
			.bytes = access_unit.bytes,
		};
}

BodyTrackingPublisher::BodyTrackingPublisher(
	std::string stream_name,
	cvmmap::NatsControlService *nats_service)
	: stream_name_(std::move(stream_name)), nats_service_(nats_service) {}

std::vector<uint8_t> BodyTrackingPublisher::Serialize(
	const cvmmap::body_tracking_frame_t &frame) const {
	auto header = frame.header;
	header._magic = cvmmap::BODY_TRACKING_MAGIC;
	header.versions_major = VERSION_MAJOR;
	header.versions_minor = VERSION_MINOR;
	std::memset(header._label, 0, sizeof(header._label));
	std::memcpy(
		header._label,
		stream_name_.data(),
		std::min(sizeof(header._label), stream_name_.size()));
	header.body_count = static_cast<uint16_t>(frame.bodies.size());
	header.body_record_size = sizeof(cvmmap::body_tracking_body_t);
	header.payload_size_bytes = static_cast<uint32_t>(
		frame.bodies.size() * sizeof(cvmmap::body_tracking_body_t));

	std::vector<uint8_t> bytes(
		sizeof(cvmmap::body_tracking_message_header_t) +
		header.payload_size_bytes);
	std::memcpy(bytes.data(), &header, sizeof(header));
	if (!frame.bodies.empty()) {
		std::memcpy(
			bytes.data() + sizeof(header),
			frame.bodies.data(),
			header.payload_size_bytes);
	}
	return bytes;
}

void BodyTrackingPublisher::Publish(
	const cvmmap::body_tracking_frame_t &frame) const {
	if (!nats_service_) {
		return;
	}
	auto bytes = Serialize(frame);
	nats_service_->PublishBodyTracking(
		std::span<const uint8_t>(bytes.data(), bytes.size()));
}

} // namespace app
