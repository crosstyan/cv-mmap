#pragma once

#include "ipc.hpp"
#include "target.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cvmmap {

struct SourceInfo {
	SourceKind source_kind{SourceKind::Unknown};
	TimestampDomain timestamp_domain{TimestampDomain::Unknown};
	uint32_t flags{0};
	uint64_t timeline_start_ns{0};
	uint64_t timeline_end_ns{0};
	uint64_t duration_ns{0};
	uint64_t current_timestamp_ns{0};
	uint32_t current_frame_count{0};

	[[nodiscard]]
	bool can_seek() const {
		return (flags & SOURCE_INFO_FLAG_CAN_SEEK) != 0;
	}

	[[nodiscard]]
	bool auto_loop() const {
		return (flags & SOURCE_INFO_FLAG_AUTO_LOOP) != 0;
	}

	[[nodiscard]]
	bool has_depth() const {
		return (flags & SOURCE_INFO_FLAG_HAS_DEPTH) != 0;
	}

	[[nodiscard]]
	bool has_body() const {
		return (flags & SOURCE_INFO_FLAG_HAS_BODY) != 0;
	}

	[[nodiscard]]
	bool can_record() const {
		return (flags & SOURCE_INFO_FLAG_CAN_RECORD) != 0;
	}
};

struct SeekResult {
	uint64_t requested_timestamp_ns{0};
	uint64_t landed_timestamp_ns{0};
	uint32_t landed_frame_count{0};
	bool exact_match{false};
};

struct RecordingStatus {
	RecordingFormat format{RecordingFormat::Unknown};
	bool can_record{false};
	bool is_recording{false};
	bool is_paused{false};
	bool last_frame_ok{false};
	uint32_t frames_ingested{0};
	uint32_t frames_encoded{0};
	std::string active_path{};
};

struct ControlError {
	int32_t code{CONTROL_RESPONSE_ERROR};
	std::string message{};
};

struct ControlCapabilities {
	bool can_seek{false};
	std::vector<RecordingFormat> available_recording_formats{};

	[[nodiscard]]
	bool supports_recording_format(RecordingFormat format) const;
};

struct SvoRecordingOptions {
	std::optional<std::string> compression_mode{};
	std::optional<uint32_t> bitrate{};
	std::optional<uint32_t> target_framerate{};
	std::optional<bool> transcode_streaming_input{};
};

struct McapRecordingOptions {
	std::optional<std::string> compression{};
	std::optional<std::string> topic{};
	std::optional<std::string> depth_topic{};
	std::optional<std::string> body_topic{};
	std::optional<std::string> frame_id{};
};

struct RecordingRequest {
	RecordingFormat format{RecordingFormat::Unknown};
	std::string output_path{};
	std::optional<SvoRecordingOptions> svo_options{};
	std::optional<McapRecordingOptions> mcap_options{};
};

struct ClientConfig {
	std::string instance_name;
	/// Control, body tracking, and module status use NATS.
	/// Frame sync stays on ZMQ PUB/SUB (unchanged).
	bool enable_nats{true};
	std::optional<std::string> nats_url{};
};

class CvMmapClient {
public:
	using OnFrameCallback = std::move_only_function<void(
		const frame_metadata_t &metadata, std::span<const uint8_t> buffer)>;
	using OnFramePlanesCallback = std::move_only_function<void(
		const frame_metadata_t &metadata, frame_planes_view_t planes)>;
	using OnBodyTrackingCallback = std::move_only_function<void(
		const body_tracking_frame_t &frame)>;
	using OnEventCallback = std::move_only_function<void(ModuleStatus status)>;

	static constexpr auto DEFAULT_CONTROL_TIMEOUT =
		std::chrono::milliseconds{1000};
	static constexpr std::string_view DEFAULT_NATS_URL =
		"nats://localhost:4222";

	explicit CvMmapClient(const std::string &instance_name);
	explicit CvMmapClient(const ClientConfig &config);
	~CvMmapClient();

	CvMmapClient(const CvMmapClient &) = delete;
	CvMmapClient &operator=(const CvMmapClient &) = delete;
	CvMmapClient(CvMmapClient &&) noexcept;
	CvMmapClient &operator=(CvMmapClient &&) noexcept;

	[[nodiscard]]
	const std::string &Name() const;
	void Start();
	void Stop();

	void SetFrameCallback(OnFrameCallback &&cb);
	void SetFramePlanesCallback(OnFramePlanesCallback &&cb);
	void SetBodyTrackingCallback(OnBodyTrackingCallback &&cb);
	void SetEventCallback(OnEventCallback &&cb);

	[[nodiscard]]
	int32_t
	ResetFrameCount(std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<SourceInfo, int32_t>
	GetSourceInfo(std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<SeekResult, int32_t>
	SeekTimestampNs(uint64_t timestamp_ns,
				   std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<ControlCapabilities, ControlError>
	GetCapabilities(std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<RecordingStatus, ControlError>
	StartRecording(const RecordingRequest &request,
				  std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<RecordingStatus, ControlError>
	StartRecording(std::string_view output_path,
				  std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<RecordingStatus, ControlError>
	StopRecording(RecordingFormat format,
				 std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<RecordingStatus, ControlError>
	StopRecording(std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<RecordingStatus, ControlError>
	GetRecordingStatus(RecordingFormat format,
						  std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<RecordingStatus, ControlError>
	GetRecordingStatus(std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

private:
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

} // namespace cvmmap
