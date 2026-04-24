#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <signal.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <zmq.hpp>

#include <cvmmap/ipc.hpp>
#include <cvmmap/nats_client.hpp>

#include "config/app_config.hpp"
#include "frame_publisher.hpp"

namespace {

struct frame_snapshot_t {
	cvmmap::frame_metadata_v2_t metadata{};
	std::vector<uint8_t> payload{};
};

struct BodyTrackingCollector {
	mutable std::mutex mutex;
	mutable std::condition_variable cv;
	std::optional<cvmmap::body_tracking_frame_t> frame{};

	void push(const cvmmap::body_tracking_frame_t &value) {
		{
			std::lock_guard lock(mutex);
			frame = value;
		}
		cv.notify_all();
	}

	[[nodiscard]]
	bool wait_for_frame(
		const std::chrono::milliseconds timeout) const {
		std::unique_lock lock(mutex);
		return cv.wait_for(lock, timeout, [&] {
			return frame.has_value();
		});
	}

	[[nodiscard]]
	cvmmap::body_tracking_frame_t value() const {
		std::lock_guard lock(mutex);
		return *frame;
	}
};

class NatsServerProcess {
public:
	NatsServerProcess()                                     = default;
	NatsServerProcess(const NatsServerProcess &)            = delete;
	NatsServerProcess &operator=(const NatsServerProcess &) = delete;

	NatsServerProcess(NatsServerProcess &&other) noexcept
		: pid_(std::exchange(other.pid_, -1)) {}

	NatsServerProcess &operator=(NatsServerProcess &&other) noexcept {
		if (this != &other) {
			Stop();
			pid_ = std::exchange(other.pid_, -1);
		}
		return *this;
	}

	~NatsServerProcess() {
		Stop();
	}

	[[nodiscard]]
	static std::optional<NatsServerProcess> Launch(const int port) {
		const pid_t child = fork();
		if (child < 0) {
			return std::nullopt;
		}
		if (child == 0) {
			const int dev_null = open("/dev/null", O_WRONLY);
			if (dev_null >= 0) {
				dup2(dev_null, STDOUT_FILENO);
				dup2(dev_null, STDERR_FILENO);
				close(dev_null);
			}
			const auto port_arg = std::to_string(port);
			execlp(
				"nats-server",
				"nats-server",
				"-a",
				"127.0.0.1",
				"-p",
				port_arg.c_str(),
				nullptr);
			_exit(127);
		}

		if (!wait_until_listening(port, std::chrono::milliseconds(3000))) {
			kill(child, SIGTERM);
			waitpid(child, nullptr, 0);
			return std::nullopt;
		}

		return NatsServerProcess(child);
	}

private:
	explicit NatsServerProcess(const pid_t pid) : pid_(pid) {}

	static bool can_connect_loopback(const int port) {
		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			return false;
		}

		sockaddr_in addr{};
		addr.sin_family      = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port        = htons(static_cast<uint16_t>(port));

		const bool connected =
			connect(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0;
		close(fd);
		return connected;
	}

	static bool wait_until_listening(
		const int port,
		const std::chrono::milliseconds timeout) {
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < deadline) {
			if (can_connect_loopback(port)) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(25));
		}
		return false;
	}

	void Stop() {
		if (pid_ <= 0) {
			return;
		}
		kill(pid_, SIGTERM);
		waitpid(pid_, nullptr, 0);
		pid_ = -1;
	}

	pid_t pid_{-1};
};

[[nodiscard]]
std::optional<int> pick_loopback_port() {
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return std::nullopt;
	}

	sockaddr_in addr{};
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
		close(fd);
		return std::nullopt;
	}

	socklen_t addr_len = sizeof(addr);
	if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) {
		close(fd);
		return std::nullopt;
	}

	const int port = static_cast<int>(ntohs(addr.sin_port));
	close(fd);
	return port;
}

[[nodiscard]]
app::Config make_test_config(std::string_view name) {
	app::Config config  = app::Config::Default();
	config.name         = std::string(name);
	config.nats.enabled = false;
	return config;
}

[[nodiscard]]
app::frame_metadata_t make_test_metadata() {
	app::frame_metadata_t metadata{};
	metadata.frame_count       = 7;
	metadata.timestamp_ns      = 123456789u;
	metadata.info.width        = 2;
	metadata.info.height       = 1;
	metadata.info.channels     = 1;
	metadata.info.depth        = app::Depth::U8;
	metadata.info.pixel_format = app::PixelFormat::GRAY;
	metadata.info.buffer_size  = 10;
	return metadata;
}

[[nodiscard]]
std::vector<uint8_t> make_depth_payload() {
	std::vector<uint8_t> payload(10);
	payload[0]                  = 0x11;
	payload[1]                  = 0x22;
	const float depth_values[2] = {1.25f, 2.5f};
	std::memcpy(payload.data() + 2, depth_values, sizeof(depth_values));
	return payload;
}

[[nodiscard]]
std::vector<uint8_t> make_encoded_payload() {
	return {0xAA, 0xBB, 0xCC, 0xDD};
}

[[nodiscard]]
app::backends::frame_payload_layout_t
make_test_left_only_layout(
	const app::frame_metadata_t &metadata,
	const size_t payload_size) {
	return app::backends::make_left_only_payload_layout(metadata, payload_size);
}

[[nodiscard]]
app::backends::frame_payload_layout_t
make_test_left_depth_layout(
	const app::frame_metadata_t &metadata,
	const app::DepthUnit depth_unit = app::DepthUnit::Meter) {
	return app::backends::make_left_depth_payload_layout(
		metadata,
		2,
		8,
		depth_unit);
}

[[nodiscard]]
std::optional<frame_snapshot_t> read_frame_snapshot(
	const app::Config &config,
	const size_t payload_size) {
	const auto shm_name = config.shm_name();
	const int fd        = shm_open(shm_name.c_str(), O_RDONLY, 0);
	if (fd == -1) {
		return std::nullopt;
	}

	const size_t total_size = cvmmap::SHM_PAYLOAD_OFFSET + payload_size;
	void *mapping           = mmap(nullptr, total_size, PROT_READ, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		close(fd);
		return std::nullopt;
	}

	frame_snapshot_t snapshot{};
	std::memcpy(&snapshot.metadata, mapping, sizeof(snapshot.metadata));
	snapshot.payload.resize(payload_size);
	if (payload_size != 0) {
		std::memcpy(
			snapshot.payload.data(),
			static_cast<const uint8_t *>(mapping) + cvmmap::SHM_PAYLOAD_OFFSET,
			payload_size);
	}

	munmap(mapping, total_size);
	close(fd);
	return snapshot;
}

[[nodiscard]]
std::optional<size_t> read_shared_payload_capacity(
	const app::Config &config) {
	const auto shm_name = config.shm_name();
	const int fd        = shm_open(shm_name.c_str(), O_RDONLY, 0);
	if (fd == -1) {
		return std::nullopt;
	}

	struct stat stat_buf{};
	if (fstat(fd, &stat_buf) == -1 ||
		stat_buf.st_size < static_cast<off_t>(cvmmap::SHM_PAYLOAD_OFFSET)) {
		close(fd);
		return std::nullopt;
	}

	const auto capacity = static_cast<size_t>(stat_buf.st_size) -
						  cvmmap::SHM_PAYLOAD_OFFSET;
	close(fd);
	return capacity;
}

[[nodiscard]]
cvmmap::body_tracking_frame_t make_body_tracking_frame() {
	cvmmap::body_tracking_frame_t frame{};
	frame.header.frame_count         = 42;
	frame.header.timestamp_ns        = 1'000;
	frame.header.sdk_timestamp_ns    = 2'000;
	frame.header.body_format         = cvmmap::BodyFormat::Body18;
	frame.header.body_selection      = cvmmap::BodyKeypointSelection::Full;
	frame.header.detection_model     = cvmmap::BodyTrackingModel::HumanBodyAccurate;
	frame.header.inference_precision = cvmmap::InferencePrecision::FP32;
	frame.header.flags               = cvmmap::BODY_TRACKING_FLAG_IS_NEW;
	frame.header.set_coordinate_system(cvmmap::BodyCoordinateSystem::RightHandedYUp);
	frame.header.set_reference_frame(cvmmap::BodyReferenceFrame::World);
	frame.header.set_floor_as_origin(true);

	cvmmap::body_tracking_body_t body{};
	body.id                     = 7;
	body.tracking_state         = cvmmap::ObjectTrackingState::Ok;
	body.action_state           = cvmmap::ObjectActionState::Idle;
	body.confidence             = 88.5f;
	body.position               = {1.0f, 2.0f, 3.0f};
	body.velocity               = {4.0f, 5.0f, 6.0f};
	body.keypoint_count         = 1;
	body.flags                  = cvmmap::BODY_TRACKING_BODY_FLAG_HAS_ROOT_ORIENTATION;
	body.bounding_box_2d[0]     = {10.0f, 11.0f};
	body.keypoint_2d[0]         = {12.0f, 13.0f};
	body.keypoint_3d[0]         = {1.0f, 2.0f, 3.0f};
	body.keypoint_confidence[0] = 0.9f;
	frame.bodies.push_back(body);
	return frame;
}

int test_direct_frame_uses_explicit_depth_unit() {
	auto payload = make_depth_payload();
	auto config  = make_test_config("fpd1");

	zmq::context_t ctx;
	zmq::socket_t pub(ctx, zmq::socket_type::pub);
	pub.bind("inproc://frame-publisher-direct");

	auto publisher = app::FramePublisher::Create(config, pub);
	if (!publisher) {
		return 1;
	}
	publisher->Configure(app::FramePublisherOptions{
		.uses_direct_frame = true,
	});

	auto metadata = make_test_metadata();
	publisher->OnMetadata(metadata);
	publisher->PublishDirectFrame(app::backends::direct_frame_t{
		.metadata     = metadata,
		.fill_payload = [payload](std::span<uint8_t> output) mutable
			-> std::optional<app::backends::direct_frame_fill_result_t> {
			if (output.size() < payload.size()) {
				return std::nullopt;
			}
			std::copy(payload.begin(), payload.end(), output.begin());
			return app::backends::direct_frame_fill_result_t{
				.payload_size_bytes = payload.size(),
				.layout             = make_test_left_depth_layout(make_test_metadata()),
			};
		},
	});

	const auto snapshot = read_frame_snapshot(config, payload.size());
	if (!snapshot) {
		return 2;
	}
	if (snapshot->metadata.header.depth_unit != cvmmap::DepthUnit::Meter) {
		return 3;
	}
	if (snapshot->metadata.header.plane_count != 2 ||
		snapshot->metadata.header.plane_presence_mask != 0x03) {
		return 4;
	}
	return 0;
}

int test_copy_frame_left_only_layout_does_not_infer_depth() {
	auto payload = make_depth_payload();
	auto config  = make_test_config("fpc1");

	zmq::context_t ctx;
	zmq::socket_t pub(ctx, zmq::socket_type::pub);
	pub.bind("inproc://frame-publisher-copy");

	auto publisher = app::FramePublisher::Create(config, pub);
	if (!publisher) {
		return 1;
	}
	publisher->Configure(app::FramePublisherOptions{
		.uses_direct_frame = false,
	});

	auto metadata = make_test_metadata();
	publisher->OnMetadata(metadata);
	const auto layout = make_test_left_only_layout(metadata, payload.size());
	publisher->PublishFrame(
		std::span<uint8_t>(payload.data(), payload.size()),
		metadata,
		layout);

	const auto snapshot = read_frame_snapshot(config, payload.size());
	if (!snapshot) {
		return 2;
	}
	if (snapshot->metadata.header.depth_unit != cvmmap::DepthUnit::Unknown) {
		return 3;
	}
	if (snapshot->metadata.header.plane_count != 1 ||
		snapshot->metadata.header.plane_presence_mask != 0x01) {
		return 4;
	}
	if (snapshot->metadata.descriptors[0].size_bytes != payload.size()) {
		return 5;
	}
	return 0;
}

int test_copy_frame_uses_explicit_depth_layout() {
	auto payload = make_depth_payload();
	auto config  = make_test_config("fpc2");

	zmq::context_t ctx;
	zmq::socket_t pub(ctx, zmq::socket_type::pub);
	pub.bind("inproc://frame-publisher-copy-depth");

	auto publisher = app::FramePublisher::Create(config, pub);
	if (!publisher) {
		return 1;
	}
	publisher->Configure(app::FramePublisherOptions{
		.uses_direct_frame = false,
	});

	auto metadata = make_test_metadata();
	publisher->OnMetadata(metadata);
	const auto layout = make_test_left_depth_layout(metadata, app::DepthUnit::Meter);
	publisher->PublishFrame(
		std::span<uint8_t>(payload.data(), payload.size()),
		metadata,
		layout);

	const auto snapshot = read_frame_snapshot(config, payload.size());
	if (!snapshot) {
		return 2;
	}
	if (snapshot->metadata.header.depth_unit != cvmmap::DepthUnit::Meter) {
		return 3;
	}
	if (snapshot->metadata.header.plane_count != 2 ||
		snapshot->metadata.header.plane_presence_mask != 0x03) {
		return 4;
	}
	const auto &depth_descriptor = snapshot->metadata.descriptors[1];
	if (depth_descriptor.plane_type != cvmmap::FramePlaneType::Depth ||
		depth_descriptor.offset_bytes != 2 ||
		depth_descriptor.size_bytes != 8) {
		return 5;
	}
	return 0;
}

int test_copy_frame_appends_encoded_access_unit() {
	const auto raw_payload     = make_depth_payload();
	const auto encoded_payload = make_encoded_payload();
	auto config                = make_test_config("fpe1");

	zmq::context_t ctx;
	zmq::socket_t pub(ctx, zmq::socket_type::pub);
	pub.bind("inproc://frame-publisher-encoded");

	auto publisher = app::FramePublisher::Create(config, pub);
	if (!publisher) {
		return 1;
	}

	auto metadata             = make_test_metadata();
	metadata.info.buffer_size = static_cast<uint32_t>(raw_payload.size());
	publisher->OnMetadata(metadata);
	publisher->OnEncodedAccessUnit(app::backends::encoded_access_unit_t{
		.codec               = cvmmap::EncodedCodec::H265,
		.bitstream_format    = cvmmap::EncodedBitstreamFormat::AnnexB,
		.flags               = 0x12,
		.frame_rate_num      = 30,
		.frame_rate_den      = 1,
		.source_timestamp_ns = metadata.timestamp_ns,
		.stream_pts_ns       = 777,
		.bytes               = encoded_payload,
	});
	const auto layout =
		make_test_left_depth_layout(metadata, app::DepthUnit::Millimeter);
	publisher->PublishFrame(
		std::span<uint8_t>(const_cast<uint8_t *>(raw_payload.data()), raw_payload.size()),
		metadata,
		layout);

	const auto total_payload_size = raw_payload.size() + encoded_payload.size();
	const auto snapshot           = read_frame_snapshot(config, total_payload_size);
	if (!snapshot) {
		return 2;
	}
	const auto &header = snapshot->metadata.header;
	if (header.versions_minor != cvmmap::FRAME_METADATA_V2_MINOR_ENCODED_AU) {
		return 3;
	}
	if (header.plane_count != 3 || header.plane_presence_mask != 0x0B) {
		return 4;
	}
	if (header.payload_size_bytes != total_payload_size) {
		return 5;
	}

	const auto &left_descriptor = snapshot->metadata.descriptors[0];
	if (left_descriptor.size_bytes != 2 ||
		left_descriptor.offset_bytes != 0) {
		return 6;
	}
	const auto &depth_descriptor = snapshot->metadata.descriptors[1];
	if (depth_descriptor.plane_type != cvmmap::FramePlaneType::Depth ||
		depth_descriptor.offset_bytes != 2 ||
		depth_descriptor.size_bytes != raw_payload.size() - 2) {
		return 7;
	}
	const auto &encoded_descriptor = snapshot->metadata.descriptors[3];
	if (encoded_descriptor.plane_type != cvmmap::FramePlaneType::EncodedAccessUnit ||
		encoded_descriptor.offset_bytes != raw_payload.size() ||
		encoded_descriptor.size_bytes != encoded_payload.size()) {
		return 8;
	}

	cvmmap::frame_metadata_v2_encoded_extension_t encoded_extension{};
	std::memcpy(
		&encoded_extension,
		header.reserved_0,
		sizeof(encoded_extension));
	if (encoded_extension.encoded_codec != cvmmap::EncodedCodec::H265 ||
		encoded_extension.encoded_bitstream_format !=
			cvmmap::EncodedBitstreamFormat::AnnexB ||
		encoded_extension.encoded_flags != 0x12 ||
		encoded_extension.encoded_frame_rate_num != 30 ||
		encoded_extension.encoded_frame_rate_den != 1 ||
		encoded_extension.encoded_stream_pts_ns != 777) {
		return 9;
	}

	if (!std::equal(
			raw_payload.begin(),
			raw_payload.end(),
			snapshot->payload.begin())) {
		return 9;
	}
	if (!std::equal(
			encoded_payload.begin(),
			encoded_payload.end(),
			snapshot->payload.begin() +
				static_cast<std::ptrdiff_t>(raw_payload.size()))) {
		return 10;
	}

	return 0;
}

int test_direct_frame_appends_encoded_access_unit() {
	const auto raw_payload     = make_depth_payload();
	const auto encoded_payload = make_encoded_payload();
	auto config                = make_test_config("fpde1");

	zmq::context_t ctx;
	zmq::socket_t pub(ctx, zmq::socket_type::pub);
	pub.bind("inproc://frame-publisher-direct-encoded");

	auto publisher = app::FramePublisher::Create(config, pub);
	if (!publisher) {
		return 1;
	}
	publisher->Configure(app::FramePublisherOptions{
		.uses_direct_frame = true,
	});

	auto metadata             = make_test_metadata();
	metadata.info.buffer_size = static_cast<uint32_t>(raw_payload.size());
	publisher->OnMetadata(metadata);
	publisher->OnEncodedAccessUnit(app::backends::encoded_access_unit_t{
		.codec               = cvmmap::EncodedCodec::H265,
		.bitstream_format    = cvmmap::EncodedBitstreamFormat::AnnexB,
		.flags               = 0x34,
		.frame_rate_num      = 60,
		.frame_rate_den      = 1,
		.source_timestamp_ns = metadata.timestamp_ns,
		.stream_pts_ns       = 888,
		.bytes               = encoded_payload,
	});
	publisher->PublishDirectFrame(app::backends::direct_frame_t{
		.metadata     = metadata,
		.fill_payload = [raw_payload, metadata](std::span<uint8_t> output)
			-> std::optional<app::backends::direct_frame_fill_result_t> {
			if (output.size() < raw_payload.size()) {
				return std::nullopt;
			}
			std::copy(raw_payload.begin(), raw_payload.end(), output.begin());
			return app::backends::direct_frame_fill_result_t{
				.payload_size_bytes = raw_payload.size(),
				.layout             = make_test_left_only_layout(metadata, raw_payload.size()),
			};
		},
	});

	const auto total_payload_size = raw_payload.size() + encoded_payload.size();
	const auto snapshot           = read_frame_snapshot(config, total_payload_size);
	if (!snapshot) {
		return 2;
	}
	const auto &header = snapshot->metadata.header;
	if (header.versions_minor != cvmmap::FRAME_METADATA_V2_MINOR_ENCODED_AU ||
		header.plane_count != 2 ||
		header.plane_presence_mask != 0x09 ||
		header.payload_size_bytes != total_payload_size) {
		return 3;
	}
	const auto &encoded_descriptor = snapshot->metadata.descriptors[3];
	if (encoded_descriptor.plane_type != cvmmap::FramePlaneType::EncodedAccessUnit ||
		encoded_descriptor.offset_bytes != raw_payload.size() ||
		encoded_descriptor.size_bytes != encoded_payload.size()) {
		return 4;
	}
	cvmmap::frame_metadata_v2_encoded_extension_t encoded_extension{};
	std::memcpy(
		&encoded_extension,
		header.reserved_0,
		sizeof(encoded_extension));
	if (encoded_extension.encoded_flags != 0x34 ||
		encoded_extension.encoded_frame_rate_num != 60 ||
		encoded_extension.encoded_stream_pts_ns != 888) {
		return 5;
	}
	if (!std::equal(
			raw_payload.begin(),
			raw_payload.end(),
			snapshot->payload.begin())) {
		return 6;
	}
	if (!std::equal(
			encoded_payload.begin(),
			encoded_payload.end(),
			snapshot->payload.begin() +
				static_cast<std::ptrdiff_t>(raw_payload.size()))) {
		return 7;
	}
	return 0;
}

int test_encoded_payload_growth_reuses_bucket_capacity() {
	auto raw_payload                 = make_depth_payload();
	const auto first_encoded_payload = make_encoded_payload();
	std::vector<uint8_t> second_encoded_payload(128, 0x5A);
	auto config = make_test_config("fpe2");

	zmq::context_t ctx;
	zmq::socket_t pub(ctx, zmq::socket_type::pub);
	pub.bind("inproc://frame-publisher-encoded-growth");

	auto publisher = app::FramePublisher::Create(config, pub);
	if (!publisher) {
		return 1;
	}

	auto metadata             = make_test_metadata();
	metadata.info.buffer_size = static_cast<uint32_t>(raw_payload.size());
	publisher->OnMetadata(metadata);
	publisher->OnEncodedAccessUnit(app::backends::encoded_access_unit_t{
		.codec               = cvmmap::EncodedCodec::H265,
		.bitstream_format    = cvmmap::EncodedBitstreamFormat::AnnexB,
		.source_timestamp_ns = metadata.timestamp_ns,
		.stream_pts_ns       = metadata.timestamp_ns,
		.bytes               = first_encoded_payload,
	});
	auto layout = make_test_left_depth_layout(metadata, app::DepthUnit::Millimeter);
	publisher->PublishFrame(
		std::span<uint8_t>(raw_payload.data(), raw_payload.size()),
		metadata,
		layout);

	const auto first_capacity = read_shared_payload_capacity(config);
	if (!first_capacity) {
		return 2;
	}

	auto second_metadata        = metadata;
	second_metadata.frame_count = 2;
	second_metadata.timestamp_ns += 1;
	publisher->OnEncodedAccessUnit(app::backends::encoded_access_unit_t{
		.codec               = cvmmap::EncodedCodec::H265,
		.bitstream_format    = cvmmap::EncodedBitstreamFormat::AnnexB,
		.source_timestamp_ns = second_metadata.timestamp_ns,
		.stream_pts_ns       = second_metadata.timestamp_ns,
		.bytes               = second_encoded_payload,
	});
	layout = make_test_left_depth_layout(second_metadata, app::DepthUnit::Millimeter);
	publisher->PublishFrame(
		std::span<uint8_t>(raw_payload.data(), raw_payload.size()),
		second_metadata,
		layout);

	const auto second_capacity = read_shared_payload_capacity(config);
	if (!second_capacity) {
		return 3;
	}
	if (*second_capacity != *first_capacity) {
		return 4;
	}

	const auto second_payload_size =
		raw_payload.size() + second_encoded_payload.size();
	const auto snapshot = read_frame_snapshot(config, second_payload_size);
	if (!snapshot) {
		return 5;
	}
	if (snapshot->metadata.header.payload_size_bytes != second_payload_size) {
		return 6;
	}
	const auto &encoded_descriptor = snapshot->metadata.descriptors[3];
	if (encoded_descriptor.offset_bytes != raw_payload.size() ||
		encoded_descriptor.size_bytes != second_encoded_payload.size()) {
		return 7;
	}
	if (!std::equal(
			second_encoded_payload.begin(),
			second_encoded_payload.end(),
			snapshot->payload.begin() +
				static_cast<std::ptrdiff_t>(raw_payload.size()))) {
		return 8;
	}

	return 0;
}

int test_invalid_layout_does_not_publish_frame() {
	auto payload = make_depth_payload();
	auto config  = make_test_config("fpi1");

	zmq::context_t ctx;
	zmq::socket_t pub(ctx, zmq::socket_type::pub);
	pub.bind("inproc://frame-publisher-invalid-layout");

	auto publisher = app::FramePublisher::Create(config, pub);
	if (!publisher) {
		return 1;
	}

	auto metadata = make_test_metadata();
	publisher->OnMetadata(metadata);
	auto layout = make_test_left_depth_layout(metadata, app::DepthUnit::Meter);
	layout.payload_size_bytes += 1;
	publisher->PublishFrame(
		std::span<uint8_t>(payload.data(), payload.size()),
		metadata,
		layout);

	const auto snapshot = read_frame_snapshot(config, metadata.info.buffer_size);
	if (!snapshot) {
		return 2;
	}
	if (snapshot->metadata.header.frame_id != 0) {
		return 3;
	}
	if (snapshot->metadata.header.plane_count != 1 ||
		snapshot->metadata.header.plane_presence_mask != 0x01) {
		return 4;
	}
	return 0;
}

int test_body_tracking_publisher_serializes_over_nats() {
	const auto port = pick_loopback_port();
	if (!port) {
		return 1;
	}
	auto server = NatsServerProcess::Launch(*port);
	if (!server) {
		return 2;
	}

	const auto nats_url          = std::string("nats://127.0.0.1:") + std::to_string(*port);
	const std::string target_key = "frame_publisher_tests_body";
	cvmmap::NatsControlService service(cvmmap::NatsControlServiceOptions{
		.instance_name  = "frame-publisher-tests",
		.namespace_name = "tests",
		.ipc_prefix     = "/tmp",
		.base_name      = "frame-publisher-tests",
		.target_key     = target_key,
		.shm_name       = "/tmp/frame-publisher-tests",
		.zmq_addr       = "ipc:///tmp/frame-publisher-tests",
		.backend        = "dummy",
		.nats_url       = nats_url,
	});
	service.SetHandlers({});
	if (!service.Start()) {
		return 3;
	}

	BodyTrackingCollector collector;
	cvmmap::NatsControlClient client(target_key, nats_url);
	client.SetBodyTrackingCallback([&collector](
									   const cvmmap::body_tracking_frame_t &frame) {
		collector.push(frame);
	});
	if (!client.Start()) {
		service.Stop();
		return 4;
	}

	app::BodyTrackingPublisher publisher("body-test-stream", &service);
	const auto frame = make_body_tracking_frame();
	for (int attempt = 0; attempt < 5; ++attempt) {
		publisher.Publish(frame);
		if (collector.wait_for_frame(std::chrono::milliseconds(200))) {
			break;
		}
	}

	if (!collector.wait_for_frame(std::chrono::milliseconds(200))) {
		client.Stop();
		service.Stop();
		return 5;
	}

	const auto received = collector.value();
	client.Stop();
	service.Stop();

	if (received.header._magic != cvmmap::BODY_TRACKING_MAGIC ||
		received.header.versions_major != cvmmap::VERSION_MAJOR ||
		received.header.versions_minor != cvmmap::VERSION_MINOR) {
		return 6;
	}
	if (received.header.label() != std::string_view{"body-test-stream"}) {
		return 7;
	}
	if (received.header.body_count != 1 ||
		received.header.body_record_size !=
			sizeof(cvmmap::body_tracking_body_t)) {
		return 8;
	}
	if (received.header.coordinate_system() !=
			cvmmap::BodyCoordinateSystem::RightHandedYUp ||
		received.header.reference_frame() !=
			cvmmap::BodyReferenceFrame::World ||
		!received.header.floor_as_origin()) {
		return 9;
	}
	if (received.bodies.size() != 1) {
		return 10;
	}
	if (received.bodies.front().id != 7 ||
		received.bodies.front().keypoint_count != 1 ||
		received.bodies.front().position[0] != 1.0f ||
		received.bodies.front().velocity[2] != 6.0f) {
		return 11;
	}

	return 0;
}

} // namespace

int main() {
	if (const auto rc = test_direct_frame_uses_explicit_depth_unit(); rc != 0) {
		return 10 + rc;
	}
	if (const auto rc = test_copy_frame_left_only_layout_does_not_infer_depth(); rc != 0) {
		return 20 + rc;
	}
	if (const auto rc = test_copy_frame_uses_explicit_depth_layout(); rc != 0) {
		return 30 + rc;
	}
	if (const auto rc = test_copy_frame_appends_encoded_access_unit(); rc != 0) {
		return 40 + rc;
	}
	if (const auto rc = test_direct_frame_appends_encoded_access_unit(); rc != 0) {
		return 50 + rc;
	}
	if (const auto rc = test_encoded_payload_growth_reuses_bucket_capacity(); rc != 0) {
		return 60 + rc;
	}
	if (const auto rc = test_invalid_layout_does_not_publish_frame(); rc != 0) {
		return 70 + rc;
	}
	if (const auto rc = test_body_tracking_publisher_serializes_over_nats(); rc != 0) {
		return 80 + rc;
	}
	return 0;
}
