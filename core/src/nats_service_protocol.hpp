#pragma once

#include <cvmmap/client.hpp>
#include <cvmmap/compat/expected.hpp>
#include <cvmmap/ipc.hpp>

#include <chrono>
#include <initializer_list>

#include <control.pb.h>

namespace cvmmap::nats_service_detail {

namespace pb = ::cvmmap::proto;

pb::ErrorCode map_control_error_code(ControlErrorCode error_code);
pb::SourceKind to_proto_source_kind(cvmmap::SourceKind source_kind);
pb::TimestampDomain to_proto_timestamp_domain(
	cvmmap::TimestampDomain timestamp_domain);
pb::ModuleStatusCode to_proto_module_status(ModuleStatus status);

void fill_camera_control_state(
	pb::CameraControlState &wire_state,
	const CameraControlState &state);
void fill_camera_control_capabilities_response(
	pb::GetCameraControlCapabilitiesResponse &response,
	const CameraControlCapabilities &capabilities);
cvmmap::expected<CameraControlSetting, ControlError> parse_camera_control_setting(
	pb::CameraControlSetting setting);
cvmmap::expected<CameraControlRequest, ControlError> parse_camera_control_request(
	const pb::SetCameraControlRequest &request);
cvmmap::expected<CameraControlRangeRequest, ControlError>
parse_camera_control_range_request(
	const pb::SetCameraControlRangeRequest &request);

void fill_svo_recording_status_response(
	pb::RecordingStatusResponse &response,
	const SvoRecordingStatus &status);
void fill_recording_capabilities_response(
	pb::CapabilitiesResponse &response,
	std::initializer_list<RecordingFormat> available_formats);

void fill_playlist_info(
	pb::PlaylistInfo &wire_info,
	const PlaylistInfo &info);
cvmmap::expected<PlaylistRequest, ControlError> parse_playlist_request(
	const pb::ApplyPlaylistRequest &request);
cvmmap::expected<SvoRecordingRequest, ControlError> parse_svo_recording_request(
	const pb::RecordingStartRequest &request);

uint64_t now_ns();

} // namespace cvmmap::nats_service_detail
