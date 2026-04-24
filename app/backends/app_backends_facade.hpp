#ifndef C56C8359_242F_4112_AFD8_5ED905EF2FA8
#define C56C8359_242F_4112_AFD8_5ED905EF2FA8

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cvmmap/backend_types.hpp>
#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/functional.hpp>
#include <cvmmap/ipc.hpp>
#include <proxy/proxy.h>

#include "app_metadata_models.hpp"

namespace app::backends {

using on_metadata_fn_t = cvmmap::move_only_function<void(const frame_metadata_t &metadata)>;
using on_frame_fn_t = cvmmap::move_only_function<void(std::span<uint8_t> frame_buffer, const frame_metadata_t &metadata)>;
using on_body_tracking_fn_t = cvmmap::move_only_function<void(const cvmmap::body_tracking_frame_t &frame)>;
using on_error_fn_t = cvmmap::move_only_function<void(error_t error_code, std::string_view message)>;

using direct_frame_fill_fn_t =
	cvmmap::move_only_function<std::optional<size_t>(std::span<uint8_t> output_buffer)>;

struct direct_frame_t {
	frame_metadata_t metadata{};
	DepthUnit depth_unit{DepthUnit::Unknown};
	direct_frame_fill_fn_t fill_payload{};
};

using on_direct_frame_fn_t = cvmmap::move_only_function<void(direct_frame_t frame)>;

struct encoded_access_unit_t {
	cvmmap::EncodedCodec codec{cvmmap::EncodedCodec::Unknown};
	cvmmap::EncodedBitstreamFormat bitstream_format{cvmmap::EncodedBitstreamFormat::Unknown};
	uint16_t flags{0};
	uint16_t frame_rate_num{0};
	uint16_t frame_rate_den{0};
	uint64_t source_timestamp_ns{0};
	uint64_t stream_pts_ns{0};
	std::vector<uint8_t> bytes{};
};

using on_encoded_access_unit_fn_t =
	cvmmap::move_only_function<void(const encoded_access_unit_t &access_unit)>;

constexpr error_t ERR_SKIP_PLAYLIST_ITEM = 1;

PRO_DEF_MEM_DISPATCH(MemInit, Init);
PRO_DEF_MEM_DISPATCH(MemShutdown, Shutdown);
PRO_DEF_MEM_DISPATCH(MemSetOnMetadata, SetOnMetadata);
PRO_DEF_MEM_DISPATCH(MemSetOnFrame, SetOnFrame);
PRO_DEF_MEM_DISPATCH(MemSetOnBodyTracking, SetOnBodyTracking);
PRO_DEF_MEM_DISPATCH(MemSetOnError, SetOnError);
PRO_DEF_MEM_DISPATCH(MemGetSourceInfo, GetSourceInfo);
PRO_DEF_MEM_DISPATCH(MemResetFrameCount, ResetFrameCount);
PRO_DEF_MEM_DISPATCH(MemStartSvoRecording, StartRecording);
PRO_DEF_MEM_DISPATCH(MemStopRecording, StopRecording);
PRO_DEF_MEM_DISPATCH(MemGetRecordingStatus, GetRecordingStatus);
PRO_DEF_MEM_DISPATCH(MemGetLastRecordingError, GetLastRecordingError);
PRO_DEF_MEM_DISPATCH(MemSetOnDirectFrame, SetOnDirectFrame);
PRO_DEF_MEM_DISPATCH(MemOnDirectOutputBufferWillReset, OnDirectOutputBufferWillReset);
PRO_DEF_MEM_DISPATCH(MemGetCameraControlCapabilities, GetCameraControlCapabilities);
PRO_DEF_MEM_DISPATCH(MemGetCameraControl, GetCameraControl);
PRO_DEF_MEM_DISPATCH(MemSetCameraControl, SetCameraControl);
PRO_DEF_MEM_DISPATCH(MemSetCameraControlRange, SetCameraControlRange);
PRO_DEF_MEM_DISPATCH(MemSetOnEncodedAccessUnit, SetOnEncodedAccessUnit);

// clang-format off
struct IBodyTrackingBackend : pro::facade_builder
	::support_copy<pro::constraint_level::nontrivial>
	::add_convention<MemSetOnBodyTracking, void(on_body_tracking_fn_t)>
	::build {};

struct IDirectFrameBackend : pro::facade_builder
	::support_copy<pro::constraint_level::nontrivial>
	::add_convention<MemSetOnDirectFrame, void(on_direct_frame_fn_t)>
	::add_convention<MemOnDirectOutputBufferWillReset, void(std::span<const uint8_t>)>
	::build {};

struct ICameraControlBackend : pro::facade_builder
	::support_copy<pro::constraint_level::nontrivial>
	::add_convention<MemGetCameraControlCapabilities, camera_control_capabilities_t()>
	::add_convention<MemGetCameraControl, cvmmap::expected<camera_control_state_t, error_t>(cvmmap::CameraControlSetting)>
	::add_convention<MemSetCameraControl, cvmmap::expected<camera_control_state_t, error_t>(const camera_control_request_t &)>
	::add_convention<MemSetCameraControlRange, cvmmap::expected<camera_control_state_t, error_t>(const camera_control_range_request_t &)>
	::build {};

struct ISvoRecordableBackend : pro::facade_builder
	::support_copy<pro::constraint_level::nontrivial>
	::add_convention<MemStartSvoRecording, cvmmap::expected<recording_status_t, error_t>(const svo_recording_request_t &)>
	::add_convention<MemStopRecording, cvmmap::expected<recording_status_t, error_t>()>
	::add_convention<MemGetRecordingStatus, cvmmap::expected<recording_status_t, error_t>()>
	::add_convention<MemGetLastRecordingError, std::string()>
	::build {};

struct IEncodedAccessUnitBackend : pro::facade_builder
	::support_copy<pro::constraint_level::nontrivial>
	::add_convention<MemSetOnEncodedAccessUnit, void(on_encoded_access_unit_fn_t)>
	::build {};

struct IBackend : pro::facade_builder
	::support_copy<pro::constraint_level::nontrivial>
	::add_convention<MemInit, void()>
	::add_convention<MemShutdown, void()>
	::add_convention<MemSetOnMetadata, void(on_metadata_fn_t)>
	::add_convention<MemSetOnFrame, void(on_frame_fn_t)>
	::add_convention<MemSetOnError, void(on_error_fn_t)>
	::add_convention<MemGetSourceInfo, source_info_t()>
	::add_convention<MemResetFrameCount, error_t()>
	::build {};
// clang-format on

} // namespace app::backends

#endif /* C56C8359_242F_4112_AFD8_5ED905EF2FA8 */
