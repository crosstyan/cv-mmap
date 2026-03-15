#pragma once

#include "ipc.hpp"
#include "target.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

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

	explicit CvMmapClient(const std::string &instance_name);
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
	std::expected<RecordingStatus, int32_t>
	StartRecording(std::string_view output_path,
				  std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<RecordingStatus, int32_t>
	StopRecording(std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

	[[nodiscard]]
	std::expected<RecordingStatus, int32_t>
	GetRecordingStatus(std::chrono::milliseconds timeout = DEFAULT_CONTROL_TIMEOUT);

private:
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

} // namespace cvmmap
