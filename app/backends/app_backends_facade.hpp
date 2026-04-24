#ifndef C56C8359_242F_4112_AFD8_5ED905EF2FA8
#define C56C8359_242F_4112_AFD8_5ED905EF2FA8

#include <algorithm>
#include <array>
#include <cstddef>
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

struct frame_payload_plane_t {
	FramePlaneType plane_type{FramePlaneType::LEFT};
	frame_info_t info{};
	size_t offset_bytes{0};
	size_t stride_bytes{0};
	size_t size_bytes{0};
};

struct frame_payload_layout_t {
	static constexpr size_t SLOT_LEFT = 0;
	static constexpr size_t SLOT_DEPTH = 1;
	static constexpr size_t SLOT_CONFIDENCE = 2;
	static constexpr size_t SLOT_COUNT = 3;

	std::array<std::optional<frame_payload_plane_t>, SLOT_COUNT> planes{};
	size_t payload_size_bytes{0};
	DepthUnit depth_unit{DepthUnit::Unknown};
};

struct direct_frame_fill_result_t {
	size_t payload_size_bytes{0};
	frame_payload_layout_t layout{};
};

[[nodiscard]]
inline size_t frame_payload_stride_or_min(
	const size_t plane_size,
	const uint32_t height,
	const size_t expected_min_stride) {
	if (height == 0) {
		return expected_min_stride;
	}
	if (plane_size % height == 0) {
		return std::max(expected_min_stride, plane_size / height);
	}
	return expected_min_stride;
}

[[nodiscard]]
inline size_t frame_info_min_stride_bytes(const frame_info_t &info) {
	const auto channel_size = size_of(info.depth);
	if (channel_size <= 0) {
		return 0;
	}
	return static_cast<size_t>(info.width) *
		   static_cast<size_t>(info.channels) *
		   static_cast<size_t>(channel_size);
}

[[nodiscard]]
inline frame_payload_plane_t make_left_payload_plane(
	frame_metadata_t metadata,
	const size_t offset,
	const size_t size) {
	metadata.info.buffer_size = static_cast<uint32_t>(size);
	return frame_payload_plane_t{
		.plane_type = FramePlaneType::LEFT,
		.info = metadata.info,
		.offset_bytes = offset,
		.stride_bytes = frame_payload_stride_or_min(
			size,
			metadata.info.height,
			frame_info_min_stride_bytes(metadata.info)),
		.size_bytes = size,
	};
}

[[nodiscard]]
inline frame_payload_plane_t make_aux_f32_payload_plane(
	const FramePlaneType plane_type,
	const frame_metadata_t &metadata,
	const size_t offset,
	const size_t size) {
	frame_info_t info{};
	info.width = metadata.info.width;
	info.height = metadata.info.height;
	info.channels = 1;
	info.depth = Depth::F32;
	info.pixel_format = PixelFormat::GRAY;
	info.buffer_size = static_cast<uint32_t>(size);
	const size_t expected_stride =
		static_cast<size_t>(metadata.info.width) * sizeof(float);
	return frame_payload_plane_t{
		.plane_type = plane_type,
		.info = info,
		.offset_bytes = offset,
		.stride_bytes = frame_payload_stride_or_min(
			size,
			metadata.info.height,
			expected_stride),
		.size_bytes = size,
	};
}

[[nodiscard]]
inline frame_payload_layout_t make_left_only_payload_layout(
	const frame_metadata_t &metadata,
	const size_t payload_size) {
	frame_payload_layout_t layout{};
	layout.payload_size_bytes = payload_size;
	layout.planes[frame_payload_layout_t::SLOT_LEFT] =
		make_left_payload_plane(metadata, 0, payload_size);
	return layout;
}

[[nodiscard]]
inline frame_payload_layout_t make_left_depth_payload_layout(
	const frame_metadata_t &metadata,
	const size_t left_size,
	const size_t depth_size,
	const DepthUnit depth_unit) {
	auto layout = make_left_only_payload_layout(metadata, left_size);
	layout.payload_size_bytes = left_size + depth_size;
	layout.depth_unit = depth_unit;
	layout.planes[frame_payload_layout_t::SLOT_DEPTH] =
		make_aux_f32_payload_plane(
			FramePlaneType::DEPTH,
			metadata,
			left_size,
			depth_size);
	return layout;
}

[[nodiscard]]
inline frame_payload_layout_t make_left_depth_confidence_payload_layout(
	const frame_metadata_t &metadata,
	const size_t left_size,
	const size_t depth_size,
	const size_t confidence_size,
	const DepthUnit depth_unit) {
	auto layout = make_left_depth_payload_layout(
		metadata,
		left_size,
		depth_size,
		depth_unit);
	layout.payload_size_bytes = left_size + depth_size + confidence_size;
	layout.planes[frame_payload_layout_t::SLOT_CONFIDENCE] =
		make_aux_f32_payload_plane(
			FramePlaneType::CONFIDENCE,
			metadata,
			left_size + depth_size,
			confidence_size);
	return layout;
}

using on_metadata_fn_t = cvmmap::move_only_function<void(const frame_metadata_t &metadata)>;
using on_frame_fn_t = cvmmap::move_only_function<void(
	std::span<uint8_t> frame_buffer,
	const frame_metadata_t &metadata,
	const frame_payload_layout_t &layout)>;
using on_body_tracking_fn_t = cvmmap::move_only_function<void(const cvmmap::body_tracking_frame_t &frame)>;
using on_error_fn_t = cvmmap::move_only_function<void(error_t error_code, std::string_view message)>;

using direct_frame_fill_fn_t =
	cvmmap::move_only_function<std::optional<direct_frame_fill_result_t>(std::span<uint8_t> output_buffer)>;

struct direct_frame_t {
	frame_metadata_t metadata{};
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
