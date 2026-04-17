#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <cvmmap/ipc.hpp>

namespace app::backends {

/// @brief POSIX style error code
using error_t = int;

constexpr error_t ERR_OK  = 0;
constexpr error_t ERR_EOS = ERR_OK;

constexpr error_t ERR_FATAL_CAMERA_RECOVERY = -0x7001;

struct source_info_t {
	cvmmap::SourceKind source_kind{cvmmap::SourceKind::Unknown};
	cvmmap::TimestampDomain timestamp_domain{cvmmap::TimestampDomain::Unknown};
	uint32_t flags{0};
	uint64_t timeline_start_ns{0};
	uint64_t timeline_end_ns{0};
	uint64_t duration_ns{0};
	uint64_t current_timestamp_ns{0};
	uint32_t current_frame_count{0};
};

struct seek_result_t {
	uint64_t requested_timestamp_ns{0};
	uint64_t landed_timestamp_ns{0};
	uint32_t landed_frame_count{0};
	bool exact_match{false};
};

struct recording_status_t {
	cvmmap::RecordingFormat format{cvmmap::RecordingFormat::Unknown};
	bool can_record{false};
	bool is_recording{false};
	bool is_paused{false};
	bool last_frame_ok{false};
	uint32_t frames_ingested{0};
	uint32_t frames_encoded{0};
	std::string active_path{};
};

struct svo_recording_options_t {
	std::optional<std::string> compression_mode{};
	std::optional<uint32_t> bitrate{};
	std::optional<uint32_t> target_framerate{};
	std::optional<bool> transcode_streaming_input{};
};

struct svo_recording_request_t {
	std::string output_path{};
	svo_recording_options_t options{};
};

} // namespace app::backends
