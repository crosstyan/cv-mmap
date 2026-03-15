#ifndef C56C8359_242F_4112_AFD8_5ED905EF2FA8
#define C56C8359_242F_4112_AFD8_5ED905EF2FA8
#include <functional>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include "proxy/v4/proxy.h"
#include <proxy/proxy.h>
#include "app_metadata_models.hpp"
#include <cvmmap/ipc.hpp>

namespace app::backends {
/// @brief POSIX style error code
using error_t = int;

constexpr error_t ERR_OK  = 0;
constexpr error_t ERR_EOS = ERR_OK;

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

/// @brief Callback invoked once when metadata is available (first frame captured)
using on_metadata_fn_t = std::move_only_function<void(const frame_metadata_t &metadata)>;
/// @brief Callback invoked for each captured frame with frame buffer and current metadata
using on_frame_fn_t = std::move_only_function<void(std::span<uint8_t> frame_buffer, const frame_metadata_t &metadata)>;
using on_body_tracking_fn_t = std::move_only_function<void(const cvmmap::body_tracking_frame_t &frame)>;
/// @brief Callback invoked on backend errors (e.g., capture failure, device disconnection)
using on_error_fn_t = std::move_only_function<void(error_t error_code, std::string_view message)>;

PRO_DEF_MEM_DISPATCH(MemInit, Init);
PRO_DEF_MEM_DISPATCH(MemShutdown, Shutdown);
PRO_DEF_MEM_DISPATCH(MemSetOnMetadata, SetOnMetadata);
PRO_DEF_MEM_DISPATCH(MemSetOnFrame, SetOnFrame);
PRO_DEF_MEM_DISPATCH(MemSetOnBodyTracking, SetOnBodyTracking);
PRO_DEF_MEM_DISPATCH(MemSetOnError, SetOnError);
PRO_DEF_MEM_DISPATCH(MemGetSourceInfo, GetSourceInfo);
PRO_DEF_MEM_DISPATCH(MemSeekTimestampNs, SeekTimestampNs);
PRO_DEF_MEM_DISPATCH(MemResetFrameCount, ResetFrameCount);
PRO_DEF_MEM_DISPATCH(MemStartRecording, StartRecording);
PRO_DEF_MEM_DISPATCH(MemStopRecording, StopRecording);
PRO_DEF_MEM_DISPATCH(MemGetRecordingStatus, GetRecordingStatus);

// clang-format off
struct IBackend : pro::facade_builder 
    ::add_convention<MemInit, void()>
    ::add_convention<MemShutdown, void()>
    ::add_convention<MemSetOnMetadata, void(on_metadata_fn_t)>
    ::add_convention<MemSetOnFrame, void(on_frame_fn_t)>
    ::add_convention<MemSetOnBodyTracking, void(on_body_tracking_fn_t)>
    ::add_convention<MemSetOnError, void(on_error_fn_t)>
    ::add_convention<MemGetSourceInfo, source_info_t()>
    ::add_convention<MemSeekTimestampNs, std::expected<seek_result_t, error_t>(uint64_t)>
    ::add_convention<MemResetFrameCount, error_t()>
    ::add_convention<MemStartRecording, std::expected<recording_status_t, error_t>(std::string_view)>
    ::add_convention<MemStopRecording, std::expected<recording_status_t, error_t>()>
    ::add_convention<MemGetRecordingStatus, std::expected<recording_status_t, error_t>()>
    ::build {};
// clang-format on

}

#endif /* C56C8359_242F_4112_AFD8_5ED905EF2FA8 */
