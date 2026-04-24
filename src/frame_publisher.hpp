#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <cvmmap/compat/expected.hpp>
#include <cvmmap/nats_service.hpp>
#include <zmq.hpp>

#include "config/app_config.hpp"
#include "models/app_metadata_models.hpp"
#include "backends/app_backends_facade.hpp"
#include "app_preprocess_undistort.hpp"

namespace app {

struct FramePublisherOptions {
	bool uses_direct_frame{false};
};

class FramePublisher {
public:
	using direct_buffer_reset_hook_t = std::function<void(std::span<const uint8_t>)>;

	static cvmmap::expected<FramePublisher, std::string>
	Create(const Config &config, zmq::socket_t &sync_socket);

	FramePublisher(FramePublisher &&other) noexcept;
	FramePublisher &operator=(FramePublisher &&other) noexcept;
	~FramePublisher();

	FramePublisher(const FramePublisher &) = delete;
	FramePublisher &operator=(const FramePublisher &) = delete;

	void Configure(FramePublisherOptions options);
	void Reset();
	void OnMetadata(
		const frame_metadata_t &metadata,
		direct_buffer_reset_hook_t before_reset = {});
	void PublishFrame(
		std::span<uint8_t> frame_buffer,
		const frame_metadata_t &metadata,
		const backends::frame_payload_layout_t &layout);
	void PublishDirectFrame(
		backends::direct_frame_t frame,
		direct_buffer_reset_hook_t before_reset = {});
	void OnEncodedAccessUnit(const backends::encoded_access_unit_t &access_unit);

private:
	struct shm_state_t {
		shm_state_t(std::string name, int shm_fd);
		~shm_state_t();
		shm_state_t(const shm_state_t &) = delete;
		shm_state_t &operator=(const shm_state_t &) = delete;
		shm_state_t(shm_state_t &&other) noexcept;
		shm_state_t &operator=(shm_state_t &&other) noexcept;

		static cvmmap::expected<shm_state_t, int> open(const std::string &name);

		std::string name{};
		int shm_fd{-1};
	};

	struct frame_state_t {
		explicit frame_state_t(std::span<uint8_t> buf);
		~frame_state_t();
		frame_state_t(const frame_state_t &) = delete;
		frame_state_t &operator=(const frame_state_t &) = delete;
		frame_state_t(frame_state_t &&other) noexcept;
		frame_state_t &operator=(frame_state_t &&other) noexcept;

		static cvmmap::expected<frame_state_t, int> open(int shm_fd, size_t size);
		[[nodiscard]] size_t total_buffer_size() const;
		[[nodiscard]] frame_metadata_v2_t &metadata();
		void write_metadata(const frame_metadata_v2_t &metadata_value);

		uint8_t *mmap_ptr{};
		std::span<uint8_t> metadata_buffer{};
		std::span<uint8_t> image_buffer{};
	};

	struct pending_encoded_access_unit_t {
		cvmmap::EncodedCodec codec{cvmmap::EncodedCodec::Unknown};
		cvmmap::EncodedBitstreamFormat bitstream_format{
			cvmmap::EncodedBitstreamFormat::Unknown};
		uint16_t flags{0};
		uint16_t frame_rate_num{0};
		uint16_t frame_rate_den{0};
		uint64_t source_timestamp_ns{0};
		uint64_t stream_pts_ns{0};
		std::vector<uint8_t> bytes{};
	};

	struct encoded_plane_view_t {
		cvmmap::EncodedCodec codec{cvmmap::EncodedCodec::Unknown};
		cvmmap::EncodedBitstreamFormat bitstream_format{
			cvmmap::EncodedBitstreamFormat::Unknown};
		uint16_t flags{0};
		uint16_t frame_rate_num{0};
		uint16_t frame_rate_den{0};
		uint64_t stream_pts_ns{0};
		std::span<const uint8_t> bytes{};
	};

	FramePublisher(
		const Config &config,
		zmq::socket_t &sync_socket,
		shm_state_t shm_state,
		std::optional<preprocess::UndistortPass> undistort_pass);

	[[nodiscard]] uint64_t now_ns() const;
	[[nodiscard]] std::optional<uint32_t> to_u32(size_t value) const;
	[[nodiscard]] std::optional<encoded_plane_view_t>
	TakeEncodedPlane(uint64_t timestamp_ns, std::vector<uint8_t> &storage);
	[[nodiscard]] std::optional<frame_metadata_v2_t> BuildV2Metadata(
		const frame_metadata_t &source_metadata,
		const backends::frame_payload_layout_t &layout,
		const std::optional<encoded_plane_view_t> &encoded_plane = std::nullopt) const;
	void EnsureMetadataState(
		const frame_metadata_t &metadata,
		size_t payload_size,
		direct_buffer_reset_hook_t before_reset);
	void SendSyncMessage(uint32_t frame_count, uint64_t timestamp_ns);

	const Config *config_{};
	zmq::socket_t *sync_socket_{};
	std::optional<shm_state_t> shm_state_{};
	std::optional<frame_state_t> frame_state_{};
	std::optional<sync_message_t> sync_msg_{};
	std::mutex pending_encoded_mutex_{};
	std::unordered_map<uint64_t, pending_encoded_access_unit_t>
		pending_encoded_by_timestamp_{};
	std::optional<preprocess::UndistortPass> undistort_pass_{};
	FramePublisherOptions options_{};
};

class BodyTrackingPublisher {
public:
	BodyTrackingPublisher(
		std::string stream_name,
		cvmmap::NatsControlService *nats_service);

	void Publish(const cvmmap::body_tracking_frame_t &frame) const;

private:
	[[nodiscard]] std::vector<uint8_t>
	Serialize(const cvmmap::body_tracking_frame_t &frame) const;

	std::string stream_name_{};
	cvmmap::NatsControlService *nats_service_{};
};

} // namespace app
