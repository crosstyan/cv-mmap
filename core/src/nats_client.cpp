#include "nats_client_internal.hpp"

#include <cvmmap/nats_client.hpp>
#include <cvmmap/nats_subjects.hpp>

#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/format.hpp>
#include <nats.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <control.pb.h>

namespace cvmmap {

namespace pb = ::cvmmap::proto;

namespace {

SourceKind from_proto_source_kind(const pb::SourceKind source_kind) {
	switch (source_kind) {
	case pb::SOURCE_KIND_LIVE:
		return SourceKind::Live;
	case pb::SOURCE_KIND_FINITE:
		return SourceKind::Finite;
	default:
		return SourceKind::Unknown;
	}
}

TimestampDomain from_proto_timestamp_domain(const pb::TimestampDomain timestamp_domain) {
	switch (timestamp_domain) {
	case pb::TIMESTAMP_DOMAIN_UNIX_EPOCH_NS:
		return TimestampDomain::UnixEpochNs;
	case pb::TIMESTAMP_DOMAIN_MEDIA_TIME_NS:
		return TimestampDomain::MediaTimeNs;
	default:
		return TimestampDomain::Unknown;
	}
}

ControlErrorCode from_proto_error_code(const pb::ErrorCode error_code) {
	switch (error_code) {
	case pb::ERROR_CODE_OK:
		return ControlErrorCode::Ok;
	case pb::ERROR_CODE_UNKNOWN_CMD:
		return ControlErrorCode::UnknownCmd;
	case pb::ERROR_CODE_UNSUPPORTED:
		return ControlErrorCode::Unsupported;
	case pb::ERROR_CODE_INVALID_PAYLOAD:
		return ControlErrorCode::InvalidPayload;
	case pb::ERROR_CODE_OUT_OF_RANGE:
		return ControlErrorCode::OutOfRange;
	case pb::ERROR_CODE_TIMEOUT:
		return ControlErrorCode::Timeout;
	default:
		return ControlErrorCode::Error;
	}
}

CameraControlSetting from_proto_camera_control_setting(
	const pb::CameraControlSetting setting) {
	switch (setting) {
	case pb::CAMERA_CONTROL_SETTING_EXPOSURE:
		return CameraControlSetting::Exposure;
	case pb::CAMERA_CONTROL_SETTING_GAIN:
		return CameraControlSetting::Gain;
	case pb::CAMERA_CONTROL_SETTING_AEC_AGC:
		return CameraControlSetting::AecAgc;
	case pb::CAMERA_CONTROL_SETTING_WHITEBALANCE_TEMPERATURE:
		return CameraControlSetting::WhitebalanceTemperature;
	case pb::CAMERA_CONTROL_SETTING_WHITEBALANCE_AUTO:
		return CameraControlSetting::WhitebalanceAuto;
	case pb::CAMERA_CONTROL_SETTING_LED_STATUS:
		return CameraControlSetting::LedStatus;
	case pb::CAMERA_CONTROL_SETTING_EXPOSURE_TIME:
		return CameraControlSetting::ExposureTime;
	case pb::CAMERA_CONTROL_SETTING_ANALOG_GAIN:
		return CameraControlSetting::AnalogGain;
	case pb::CAMERA_CONTROL_SETTING_DIGITAL_GAIN:
		return CameraControlSetting::DigitalGain;
	case pb::CAMERA_CONTROL_SETTING_AUTO_EXPOSURE_TIME_RANGE:
		return CameraControlSetting::AutoExposureTimeRange;
	case pb::CAMERA_CONTROL_SETTING_AUTO_ANALOG_GAIN_RANGE:
		return CameraControlSetting::AutoAnalogGainRange;
	case pb::CAMERA_CONTROL_SETTING_AUTO_DIGITAL_GAIN_RANGE:
		return CameraControlSetting::AutoDigitalGainRange;
	default:
		return CameraControlSetting::Unknown;
	}
}

CameraControlValueKind from_proto_camera_control_value_kind(
	const pb::CameraControlValueKind kind) {
	switch (kind) {
	case pb::CAMERA_CONTROL_VALUE_KIND_SINGLE:
		return CameraControlValueKind::Single;
	case pb::CAMERA_CONTROL_VALUE_KIND_RANGE:
		return CameraControlValueKind::Range;
	default:
		return CameraControlValueKind::Unknown;
	}
}

pb::CameraControlSetting to_proto_camera_control_setting(
	const CameraControlSetting setting) {
	switch (setting) {
	case CameraControlSetting::Exposure:
		return pb::CAMERA_CONTROL_SETTING_EXPOSURE;
	case CameraControlSetting::Gain:
		return pb::CAMERA_CONTROL_SETTING_GAIN;
	case CameraControlSetting::AecAgc:
		return pb::CAMERA_CONTROL_SETTING_AEC_AGC;
	case CameraControlSetting::WhitebalanceTemperature:
		return pb::CAMERA_CONTROL_SETTING_WHITEBALANCE_TEMPERATURE;
	case CameraControlSetting::WhitebalanceAuto:
		return pb::CAMERA_CONTROL_SETTING_WHITEBALANCE_AUTO;
	case CameraControlSetting::LedStatus:
		return pb::CAMERA_CONTROL_SETTING_LED_STATUS;
	case CameraControlSetting::ExposureTime:
		return pb::CAMERA_CONTROL_SETTING_EXPOSURE_TIME;
	case CameraControlSetting::AnalogGain:
		return pb::CAMERA_CONTROL_SETTING_ANALOG_GAIN;
	case CameraControlSetting::DigitalGain:
		return pb::CAMERA_CONTROL_SETTING_DIGITAL_GAIN;
	case CameraControlSetting::AutoExposureTimeRange:
		return pb::CAMERA_CONTROL_SETTING_AUTO_EXPOSURE_TIME_RANGE;
	case CameraControlSetting::AutoAnalogGainRange:
		return pb::CAMERA_CONTROL_SETTING_AUTO_ANALOG_GAIN_RANGE;
	case CameraControlSetting::AutoDigitalGainRange:
		return pb::CAMERA_CONTROL_SETTING_AUTO_DIGITAL_GAIN_RANGE;
	default:
		return pb::CAMERA_CONTROL_SETTING_UNKNOWN;
	}
}

pb::CameraControlWriteMode to_proto_camera_control_write_mode(
	const CameraControlWriteMode mode) {
	switch (mode) {
	case CameraControlWriteMode::Manual:
		return pb::CAMERA_CONTROL_WRITE_MODE_MANUAL;
	case CameraControlWriteMode::Auto:
		return pb::CAMERA_CONTROL_WRITE_MODE_AUTO;
	default:
		return pb::CAMERA_CONTROL_WRITE_MODE_UNKNOWN;
	}
}

CameraControlState to_camera_control_state(const pb::CameraControlState &wire_state) {
	return CameraControlState{
		.setting = from_proto_camera_control_setting(wire_state.setting()),
		.kind = from_proto_camera_control_value_kind(wire_state.kind()),
		.value = wire_state.value(),
		.min_value = wire_state.min_value(),
		.max_value = wire_state.max_value(),
	};
}

ControlError response_error(ControlErrorCode code, const std::string &message = {}) {
	return ControlError{
		.code = code,
		.message = message,
	};
}


SvoRecordingStatus to_svo_recording_status(
	const pb::RecordingStatusResponse &response) {
	return SvoRecordingStatus{
		.can_record = response.can_record(),
		.is_recording = response.is_recording(),
		.is_paused = response.is_paused(),
		.last_frame_ok = response.last_frame_ok(),
		.frames_ingested = response.frames_ingested(),
		.frames_encoded = response.frames_encoded(),
		.active_path = response.active_path(),
	};
}

PlaylistInfo to_playlist_info(const pb::PlaylistInfo &wire_info) {
	PlaylistInfo info{
		.has_playlist = wire_info.has_playlist(),
		.sort_by_recording_time = wire_info.sort_by_recording_time(),
		.current_index = wire_info.current_index(),
		.current_path = wire_info.current_path(),
	};
	info.paths.reserve(static_cast<size_t>(wire_info.paths_size()));
	for (const auto &path : wire_info.paths()) {
		info.paths.push_back(path);
	}
	return info;
}

void apply_svo_options(
	const SvoRecordingOptions &options,
	pb::SvoRecordingOptions *wire_options) {
	if (options.compression_mode) {
		wire_options->set_compression_mode(*options.compression_mode);
	}
	if (options.bitrate) {
		wire_options->set_bitrate(*options.bitrate);
	}
	if (options.target_framerate) {
		wire_options->set_target_framerate(*options.target_framerate);
	}
	if (options.transcode_streaming_input) {
		wire_options->set_transcode_streaming_input(*options.transcode_streaming_input);
	}
}

void apply_playlist_request(
	const PlaylistRequest &request,
	pb::ApplyPlaylistRequest *wire_request) {
	wire_request->set_sort_by_recording_time(request.sort_by_recording_time);
	for (const auto &path : request.paths) {
		wire_request->add_paths(path);
	}
}

} // namespace

NatsControlClient::NatsControlClient(std::string target_key, std::string nats_url)
	: pimpl_(std::make_unique<impl>()) {
	pimpl_->target_key = std::move(target_key);
	pimpl_->nats_url = std::move(nats_url);
}

NatsControlClient::~NatsControlClient() {
	Stop();
}

bool NatsControlClient::Start() {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	if (pimpl_->conn) {
		return true;
	}

	natsOptions *options = nullptr;
	natsOptions_Create(&options);
	natsOptions_SetURL(options, pimpl_->nats_url.c_str());

	const auto status = natsConnection_Connect(&pimpl_->conn, options);
	natsOptions_Destroy(options);
	if (status != NATS_OK) {
		spdlog::error(
			"nats client connect to '{}': {}",
			pimpl_->nats_url,
			natsStatus_GetText(status));
		return false;
	}

	pimpl_->refresh_body_subscription_locked();
	pimpl_->refresh_status_subscription_locked();

	spdlog::info("nats client started for target '{}'", pimpl_->target_key);
	return true;
}

void NatsControlClient::Stop() {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	pimpl_->destroy_subscription(pimpl_->sub_body);
	pimpl_->destroy_subscription(pimpl_->sub_status);

	if (pimpl_->conn) {
		natsConnection_Close(pimpl_->conn);
		natsConnection_Destroy(pimpl_->conn);
		pimpl_->conn = nullptr;
	}
}

ControlErrorCode NatsControlClient::ResetFrameCount(
	const std::chrono::milliseconds timeout) {
	pb::ResetFrameCountRequest request;
	auto response =
		pimpl_->request<pb::ResetFrameCountRequest, pb::ResetFrameCountResponse>(
			nats::subject_producer_source_reset(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return response.error();
	}
	return from_proto_error_code(response->error());
}

cvmmap::expected<SourceInfo, ControlErrorCode> NatsControlClient::GetSourceInfo(
	const std::chrono::milliseconds timeout) {
	pb::GetSourceInfoRequest request;
	auto response =
		pimpl_->request<pb::GetSourceInfoRequest, pb::GetSourceInfoResponse>(
			nats::subject_producer_source_info(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response.error());
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(from_proto_error_code(response->error()));
	}
	return SourceInfo{
		.source_kind = from_proto_source_kind(response->source_kind()),
		.timestamp_domain = from_proto_timestamp_domain(response->timestamp_domain()),
		.flags = response->flags(),
		.timeline_start_ns = response->timeline_start_ns(),
		.timeline_end_ns = response->timeline_end_ns(),
		.duration_ns = response->duration_ns(),
		.current_timestamp_ns = response->current_timestamp_ns(),
		.current_frame_count = response->current_frame_count(),
	};
}

cvmmap::expected<PlaylistInfo, ControlError> NatsControlClient::ApplyPlaylist(
	const PlaylistRequest &request,
	const std::chrono::milliseconds timeout) {
	pb::ApplyPlaylistRequest wire_request;
	apply_playlist_request(request, &wire_request);
	auto response =
		pimpl_->request<pb::ApplyPlaylistRequest, pb::ApplyPlaylistResponse>(
			nats::subject_producer_source_playlist_apply(pimpl_->target_key),
			wire_request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}
	return to_playlist_info(response->playlist_info());
}

cvmmap::expected<CameraControlCapabilities, ControlError>
NatsControlClient::GetCameraControlCapabilities(
	const std::chrono::milliseconds timeout) {
	pb::GetCameraControlCapabilitiesRequest request;
	auto response =
		pimpl_->request<
			pb::GetCameraControlCapabilitiesRequest,
			pb::GetCameraControlCapabilitiesResponse>(
				nats::subject_producer_camera_control_capabilities(
					pimpl_->target_key),
				request,
				timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}

	CameraControlCapabilities capabilities{
		.supported = response->supported(),
	};
	capabilities.supported_settings.reserve(
		static_cast<size_t>(response->supported_settings_size()));
	for (const auto setting : response->supported_settings()) {
		capabilities.supported_settings.push_back(
			from_proto_camera_control_setting(
				static_cast<pb::CameraControlSetting>(setting)));
	}
	return capabilities;
}

cvmmap::expected<CameraControlState, ControlError> NatsControlClient::GetCameraControl(
	const CameraControlSetting setting,
	const std::chrono::milliseconds timeout) {
	pb::GetCameraControlRequest request;
	request.set_setting(to_proto_camera_control_setting(setting));
	auto response =
		pimpl_->request<pb::GetCameraControlRequest, pb::GetCameraControlResponse>(
			nats::subject_producer_camera_control_get(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}
	return to_camera_control_state(response->control());
}

cvmmap::expected<CameraControlState, ControlError> NatsControlClient::SetCameraControl(
	const CameraControlRequest &request,
	const std::chrono::milliseconds timeout) {
	pb::SetCameraControlRequest wire_request;
	wire_request.set_setting(to_proto_camera_control_setting(request.setting));
	wire_request.set_mode(to_proto_camera_control_write_mode(request.mode));
	wire_request.set_value(request.value);
	auto response =
		pimpl_->request<pb::SetCameraControlRequest, pb::SetCameraControlResponse>(
			nats::subject_producer_camera_control_set(pimpl_->target_key),
			wire_request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}
	return to_camera_control_state(response->control());
}

cvmmap::expected<CameraControlState, ControlError> NatsControlClient::SetCameraControlRange(
	const CameraControlRangeRequest &request,
	const std::chrono::milliseconds timeout) {
	pb::SetCameraControlRangeRequest wire_request;
	wire_request.set_setting(to_proto_camera_control_setting(request.setting));
	wire_request.set_min_value(request.min_value);
	wire_request.set_max_value(request.max_value);
	auto response =
		pimpl_->request<
			pb::SetCameraControlRangeRequest,
			pb::SetCameraControlRangeResponse>(
				nats::subject_producer_camera_control_set_range(
					pimpl_->target_key),
				wire_request,
				timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}
	return to_camera_control_state(response->control());
}

cvmmap::expected<PlaylistInfo, ControlError> NatsControlClient::GetPlaylistInfo(
	const std::chrono::milliseconds timeout) {
	pb::GetPlaylistInfoRequest request;
	auto response =
		pimpl_->request<pb::GetPlaylistInfoRequest, pb::GetPlaylistInfoResponse>(
			nats::subject_producer_source_playlist_info(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}
	return to_playlist_info(response->playlist_info());
}

cvmmap::expected<SvoRecordingCapabilities, ControlError>
NatsControlClient::GetSvoRecordingCapabilities(
	const std::chrono::milliseconds timeout) {
	pb::CapabilitiesRequest request;
	auto response =
		pimpl_->request<pb::CapabilitiesRequest, pb::CapabilitiesResponse>(
			nats::subject_producer_svo_recorder_capabilities(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error())));
	}
	return SvoRecordingCapabilities{
		.can_record = response->available_recording_formats_size() > 0,
	};
}

cvmmap::expected<SvoRecordingStatus, ControlError>
NatsControlClient::StartSvoRecording(
	const SvoRecordingRequest &request,
	const std::chrono::milliseconds timeout) {
	if (request.output_path.empty()) {
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "recording path is empty",
		});
	}

	pb::RecordingStartRequest wire_request;
	wire_request.set_output_path(request.output_path);
	if (request.svo_options) {
		apply_svo_options(*request.svo_options, wire_request.mutable_svo_options());
	}

	auto response =
		pimpl_->request<pb::RecordingStartRequest, pb::RecordingStatusResponse>(
			nats::subject_producer_svo_recorder_start(pimpl_->target_key),
			wire_request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}
	return to_svo_recording_status(*response);
}

cvmmap::expected<SvoRecordingStatus, ControlError>
NatsControlClient::StopSvoRecording(
	const std::chrono::milliseconds timeout) {
	pb::RecordingStopRequest request;
	auto response =
		pimpl_->request<pb::RecordingStopRequest, pb::RecordingStatusResponse>(
			nats::subject_producer_svo_recorder_stop(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}
	return to_svo_recording_status(*response);
}

cvmmap::expected<SvoRecordingStatus, ControlError>
NatsControlClient::GetSvoRecordingStatus(
	const std::chrono::milliseconds timeout) {
	pb::RecordingStatusRequest request;
	auto response =
		pimpl_->request<pb::RecordingStatusRequest, pb::RecordingStatusResponse>(
			nats::subject_producer_svo_recorder_status(pimpl_->target_key),
			request,
			timeout);
	if (!response) {
		return cvmmap::unexpected(response_error(response.error()));
	}
	if (response->error() != pb::ERROR_CODE_OK) {
		return cvmmap::unexpected(response_error(
			from_proto_error_code(response->error()),
			response->error_message()));
	}
	return to_svo_recording_status(*response);
}

void NatsControlClient::SetBodyTrackingCallback(OnBodyTrackingCallback &&callback) {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	pimpl_->on_body_tracking = std::move(callback);
	pimpl_->refresh_body_subscription_locked();
}

void NatsControlClient::SetBodyTrackingRawCallback(
	OnBodyTrackingRawCallback &&callback) {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	pimpl_->on_body_tracking_raw = std::move(callback);
	pimpl_->refresh_body_subscription_locked();
}

void NatsControlClient::SetModuleStatusCallback(
	OnModuleStatusCallback &&callback) {
	std::lock_guard<std::mutex> lock(pimpl_->conn_mutex);
	pimpl_->on_module_status = std::move(callback);
	pimpl_->refresh_status_subscription_locked();
}

} // namespace cvmmap
