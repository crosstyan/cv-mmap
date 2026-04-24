#include "nats_service_protocol.hpp"

#include <chrono>

namespace cvmmap::nats_service_detail {

pb::ErrorCode map_control_error_code(const ControlErrorCode error_code) {
	switch (error_code) {
	case ControlErrorCode::Ok:
		return pb::ERROR_CODE_OK;
	case ControlErrorCode::UnknownCmd:
		return pb::ERROR_CODE_UNKNOWN_CMD;
	case ControlErrorCode::Unsupported:
		return pb::ERROR_CODE_UNSUPPORTED;
	case ControlErrorCode::InvalidPayload:
		return pb::ERROR_CODE_INVALID_PAYLOAD;
	case ControlErrorCode::OutOfRange:
		return pb::ERROR_CODE_OUT_OF_RANGE;
	case ControlErrorCode::Timeout:
		return pb::ERROR_CODE_TIMEOUT;
	default:
		return pb::ERROR_CODE_ERROR;
	}
}

pb::SourceKind to_proto_source_kind(const cvmmap::SourceKind source_kind) {
	switch (source_kind) {
	case cvmmap::SourceKind::Live:
		return pb::SOURCE_KIND_LIVE;
	case cvmmap::SourceKind::Finite:
		return pb::SOURCE_KIND_FINITE;
	default:
		return pb::SOURCE_KIND_UNKNOWN;
	}
}

pb::TimestampDomain to_proto_timestamp_domain(
	const cvmmap::TimestampDomain timestamp_domain) {
	switch (timestamp_domain) {
	case cvmmap::TimestampDomain::UnixEpochNs:
		return pb::TIMESTAMP_DOMAIN_UNIX_EPOCH_NS;
	case cvmmap::TimestampDomain::MediaTimeNs:
		return pb::TIMESTAMP_DOMAIN_MEDIA_TIME_NS;
	default:
		return pb::TIMESTAMP_DOMAIN_UNKNOWN;
	}
}

namespace {

pb::RecordingFormat to_proto_recording_format(
	const cvmmap::RecordingFormat recording_format) {
	switch (recording_format) {
	case cvmmap::RecordingFormat::Svo:
		return pb::RECORDING_FORMAT_SVO;
	default:
		return pb::RECORDING_FORMAT_UNKNOWN;
	}
}

pb::CameraControlSetting to_proto_camera_control_setting(
	const cvmmap::CameraControlSetting setting) {
	switch (setting) {
	case cvmmap::CameraControlSetting::Exposure:
		return pb::CAMERA_CONTROL_SETTING_EXPOSURE;
	case cvmmap::CameraControlSetting::Gain:
		return pb::CAMERA_CONTROL_SETTING_GAIN;
	case cvmmap::CameraControlSetting::AecAgc:
		return pb::CAMERA_CONTROL_SETTING_AEC_AGC;
	case cvmmap::CameraControlSetting::WhitebalanceTemperature:
		return pb::CAMERA_CONTROL_SETTING_WHITEBALANCE_TEMPERATURE;
	case cvmmap::CameraControlSetting::WhitebalanceAuto:
		return pb::CAMERA_CONTROL_SETTING_WHITEBALANCE_AUTO;
	case cvmmap::CameraControlSetting::LedStatus:
		return pb::CAMERA_CONTROL_SETTING_LED_STATUS;
	case cvmmap::CameraControlSetting::ExposureTime:
		return pb::CAMERA_CONTROL_SETTING_EXPOSURE_TIME;
	case cvmmap::CameraControlSetting::AnalogGain:
		return pb::CAMERA_CONTROL_SETTING_ANALOG_GAIN;
	case cvmmap::CameraControlSetting::DigitalGain:
		return pb::CAMERA_CONTROL_SETTING_DIGITAL_GAIN;
	case cvmmap::CameraControlSetting::AutoExposureTimeRange:
		return pb::CAMERA_CONTROL_SETTING_AUTO_EXPOSURE_TIME_RANGE;
	case cvmmap::CameraControlSetting::AutoAnalogGainRange:
		return pb::CAMERA_CONTROL_SETTING_AUTO_ANALOG_GAIN_RANGE;
	case cvmmap::CameraControlSetting::AutoDigitalGainRange:
		return pb::CAMERA_CONTROL_SETTING_AUTO_DIGITAL_GAIN_RANGE;
	default:
		return pb::CAMERA_CONTROL_SETTING_UNKNOWN;
	}
}

cvmmap::CameraControlSetting from_proto_camera_control_setting(
	const pb::CameraControlSetting setting) {
	switch (setting) {
	case pb::CAMERA_CONTROL_SETTING_EXPOSURE:
		return cvmmap::CameraControlSetting::Exposure;
	case pb::CAMERA_CONTROL_SETTING_GAIN:
		return cvmmap::CameraControlSetting::Gain;
	case pb::CAMERA_CONTROL_SETTING_AEC_AGC:
		return cvmmap::CameraControlSetting::AecAgc;
	case pb::CAMERA_CONTROL_SETTING_WHITEBALANCE_TEMPERATURE:
		return cvmmap::CameraControlSetting::WhitebalanceTemperature;
	case pb::CAMERA_CONTROL_SETTING_WHITEBALANCE_AUTO:
		return cvmmap::CameraControlSetting::WhitebalanceAuto;
	case pb::CAMERA_CONTROL_SETTING_LED_STATUS:
		return cvmmap::CameraControlSetting::LedStatus;
	case pb::CAMERA_CONTROL_SETTING_EXPOSURE_TIME:
		return cvmmap::CameraControlSetting::ExposureTime;
	case pb::CAMERA_CONTROL_SETTING_ANALOG_GAIN:
		return cvmmap::CameraControlSetting::AnalogGain;
	case pb::CAMERA_CONTROL_SETTING_DIGITAL_GAIN:
		return cvmmap::CameraControlSetting::DigitalGain;
	case pb::CAMERA_CONTROL_SETTING_AUTO_EXPOSURE_TIME_RANGE:
		return cvmmap::CameraControlSetting::AutoExposureTimeRange;
	case pb::CAMERA_CONTROL_SETTING_AUTO_ANALOG_GAIN_RANGE:
		return cvmmap::CameraControlSetting::AutoAnalogGainRange;
	case pb::CAMERA_CONTROL_SETTING_AUTO_DIGITAL_GAIN_RANGE:
		return cvmmap::CameraControlSetting::AutoDigitalGainRange;
	default:
		return cvmmap::CameraControlSetting::Unknown;
	}
}

pb::CameraControlValueKind to_proto_camera_control_value_kind(
	const cvmmap::CameraControlValueKind kind) {
	switch (kind) {
	case cvmmap::CameraControlValueKind::Single:
		return pb::CAMERA_CONTROL_VALUE_KIND_SINGLE;
	case cvmmap::CameraControlValueKind::Range:
		return pb::CAMERA_CONTROL_VALUE_KIND_RANGE;
	default:
		return pb::CAMERA_CONTROL_VALUE_KIND_UNKNOWN;
	}
}

cvmmap::CameraControlWriteMode from_proto_camera_control_write_mode(
	const pb::CameraControlWriteMode mode) {
	switch (mode) {
	case pb::CAMERA_CONTROL_WRITE_MODE_MANUAL:
		return cvmmap::CameraControlWriteMode::Manual;
	case pb::CAMERA_CONTROL_WRITE_MODE_AUTO:
		return cvmmap::CameraControlWriteMode::Auto;
	default:
		return cvmmap::CameraControlWriteMode::Unknown;
	}
}

} // namespace

pb::ModuleStatusCode to_proto_module_status(const ModuleStatus status) {
	if (status == ModuleStatus::Online) {
		return pb::MODULE_STATUS_CODE_ONLINE;
	}
	if (status == ModuleStatus::Offline) {
		return pb::MODULE_STATUS_CODE_OFFLINE;
	}
	if (status == ModuleStatus::StreamReset) {
		return pb::MODULE_STATUS_CODE_STREAM_RESET;
	}
	return pb::MODULE_STATUS_CODE_UNKNOWN;
}

void fill_camera_control_state(
	pb::CameraControlState &wire_state,
	const CameraControlState &state) {
	wire_state.set_setting(to_proto_camera_control_setting(state.setting));
	wire_state.set_kind(to_proto_camera_control_value_kind(state.kind));
	wire_state.set_value(state.value);
	wire_state.set_min_value(state.min_value);
	wire_state.set_max_value(state.max_value);
}

void fill_camera_control_capabilities_response(
	pb::GetCameraControlCapabilitiesResponse &response,
	const CameraControlCapabilities &capabilities) {
	response.set_error(pb::ERROR_CODE_OK);
	response.set_supported(capabilities.supported);
	for (const auto setting : capabilities.supported_settings) {
		response.add_supported_settings(
			to_proto_camera_control_setting(setting));
	}
}

cvmmap::expected<CameraControlSetting, ControlError> parse_camera_control_setting(
	const pb::CameraControlSetting setting) {
	const auto parsed = from_proto_camera_control_setting(setting);
	if (parsed == CameraControlSetting::Unknown) {
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "camera control setting is required",
		});
	}
	return parsed;
}

cvmmap::expected<CameraControlRequest, ControlError> parse_camera_control_request(
	const pb::SetCameraControlRequest &request) {
	auto setting = parse_camera_control_setting(request.setting());
	if (!setting) {
		return cvmmap::unexpected(setting.error());
	}
	const auto mode = from_proto_camera_control_write_mode(request.mode());
	if (mode == CameraControlWriteMode::Unknown) {
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "camera control write mode is required",
		});
	}
	return CameraControlRequest{
		.setting = *setting,
		.mode = mode,
		.value = request.value(),
	};
}

cvmmap::expected<CameraControlRangeRequest, ControlError>
parse_camera_control_range_request(
	const pb::SetCameraControlRangeRequest &request) {
	auto setting = parse_camera_control_setting(request.setting());
	if (!setting) {
		return cvmmap::unexpected(setting.error());
	}
	if (request.min_value() > request.max_value()) {
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "camera control range min_value must be <= max_value",
		});
	}
	return CameraControlRangeRequest{
		.setting = *setting,
		.min_value = request.min_value(),
		.max_value = request.max_value(),
	};
}

void fill_svo_recording_status_response(
	pb::RecordingStatusResponse &response,
	const SvoRecordingStatus &status) {
	response.set_error(pb::ERROR_CODE_OK);
	response.set_format(pb::RECORDING_FORMAT_SVO);
	response.set_can_record(status.can_record);
	response.set_is_recording(status.is_recording);
	response.set_is_paused(status.is_paused);
	response.set_last_frame_ok(status.last_frame_ok);
	response.set_frames_ingested(status.frames_ingested);
	response.set_frames_encoded(status.frames_encoded);
	response.set_active_path(status.active_path);
}

void fill_recording_capabilities_response(
	pb::CapabilitiesResponse &response,
	const std::initializer_list<RecordingFormat> available_formats) {
	response.set_error(pb::ERROR_CODE_OK);
	for (const auto format : available_formats) {
		response.add_available_recording_formats(
			to_proto_recording_format(format));
	}
}

void fill_playlist_info(
	pb::PlaylistInfo &wire_info,
	const PlaylistInfo &info) {
	wire_info.set_has_playlist(info.has_playlist);
	wire_info.set_sort_by_recording_time(info.sort_by_recording_time);
	wire_info.set_current_index(info.current_index);
	wire_info.set_current_path(info.current_path);
	for (const auto &path : info.paths) {
		wire_info.add_paths(path);
	}
}

cvmmap::expected<PlaylistRequest, ControlError> parse_playlist_request(
	const pb::ApplyPlaylistRequest &request) {
	PlaylistRequest parsed{};
	parsed.sort_by_recording_time = request.sort_by_recording_time();
	parsed.paths.reserve(static_cast<size_t>(request.paths_size()));
	for (const auto &path : request.paths()) {
		if (path.empty()) {
			return cvmmap::unexpected(ControlError{
				.code = ControlErrorCode::InvalidPayload,
				.message = "playlist paths must not be empty",
			});
		}
		parsed.paths.push_back(path);
	}
	if (parsed.paths.empty()) {
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "playlist paths must not be empty",
		});
	}
	return parsed;
}

cvmmap::expected<SvoRecordingRequest, ControlError> parse_svo_recording_request(
	const pb::RecordingStartRequest &request) {
	if (request.output_path().empty()) {
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "recording path is empty",
		});
	}

	SvoRecordingRequest parsed{
		.output_path = request.output_path(),
	};

	if (request.has_mcap_options()) {
		return cvmmap::unexpected(ControlError{
			.code = ControlErrorCode::InvalidPayload,
			.message = "MCAP options are invalid for SVO recording",
		});
	}
	if (request.has_svo_options()) {
		SvoRecordingOptions options{};
		const auto &wire_options = request.svo_options();
		if (wire_options.has_compression_mode()) {
			options.compression_mode = wire_options.compression_mode();
		}
		if (wire_options.has_bitrate()) {
			options.bitrate = wire_options.bitrate();
		}
		if (wire_options.has_target_framerate()) {
			options.target_framerate = wire_options.target_framerate();
		}
		if (wire_options.has_transcode_streaming_input()) {
			options.transcode_streaming_input =
				wire_options.transcode_streaming_input();
		}
		parsed.svo_options = std::move(options);
	}

	return parsed;
}

uint64_t now_ns() {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

} // namespace cvmmap::nats_service_detail
