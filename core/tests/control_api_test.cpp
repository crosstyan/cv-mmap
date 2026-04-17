#include <cvmmap/client.hpp>
#include <cvmmap/ipc.hpp>
#include <cvmmap/nats_subjects.hpp>

#include <cstdint>
#include <iostream>

namespace {

bool test_control_error_code_values() {
	return static_cast<uint8_t>(cvmmap::ControlErrorCode::Ok) == 0 &&
		   static_cast<uint8_t>(cvmmap::ControlErrorCode::UnknownCmd) == 1 &&
		   static_cast<uint8_t>(cvmmap::ControlErrorCode::Error) == 2 &&
		   static_cast<uint8_t>(cvmmap::ControlErrorCode::Unsupported) == 3 &&
		   static_cast<uint8_t>(cvmmap::ControlErrorCode::InvalidPayload) == 4 &&
		   static_cast<uint8_t>(cvmmap::ControlErrorCode::OutOfRange) == 5 &&
		   static_cast<uint8_t>(cvmmap::ControlErrorCode::Timeout) == 6;
}

bool test_module_status_values() {
	return static_cast<uint8_t>(cvmmap::ModuleStatus::Unknown) == 0 &&
		   static_cast<uint8_t>(cvmmap::ModuleStatus::Online) == 1 &&
		   static_cast<uint8_t>(cvmmap::ModuleStatus::Offline) == 2 &&
		   static_cast<uint8_t>(cvmmap::ModuleStatus::StreamReset) == 3;
}

bool test_control_error_defaults() {
	const cvmmap::ControlError error{};
	return error.code == cvmmap::ControlErrorCode::Error &&
		   error.message.empty();
}

bool test_camera_control_enum_values() {
	return static_cast<uint8_t>(cvmmap::CameraControlSetting::Unknown) == 0 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::Exposure) == 1 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::Gain) == 2 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::AecAgc) == 3 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::WhitebalanceTemperature) == 4 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::WhitebalanceAuto) == 5 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::LedStatus) == 6 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::ExposureTime) == 7 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::AnalogGain) == 8 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::DigitalGain) == 9 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::AutoExposureTimeRange) == 10 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::AutoAnalogGainRange) == 11 &&
		   static_cast<uint8_t>(cvmmap::CameraControlSetting::AutoDigitalGainRange) == 12 &&
		   static_cast<uint8_t>(cvmmap::CameraControlValueKind::Unknown) == 0 &&
		   static_cast<uint8_t>(cvmmap::CameraControlValueKind::Single) == 1 &&
		   static_cast<uint8_t>(cvmmap::CameraControlValueKind::Range) == 2 &&
		   static_cast<uint8_t>(cvmmap::CameraControlWriteMode::Unknown) == 0 &&
		   static_cast<uint8_t>(cvmmap::CameraControlWriteMode::Manual) == 1 &&
		   static_cast<uint8_t>(cvmmap::CameraControlWriteMode::Auto) == 2;
}

bool test_camera_control_type_defaults() {
	const cvmmap::CameraControlCapabilities capabilities{};
	const cvmmap::CameraControlState state{};
	const cvmmap::CameraControlRequest set_request{};
	const cvmmap::CameraControlRangeRequest set_range_request{};

	return !capabilities.supported &&
		   capabilities.supported_settings.empty() &&
		   state.setting == cvmmap::CameraControlSetting::Unknown &&
		   state.kind == cvmmap::CameraControlValueKind::Unknown &&
		   !state.is_single() &&
		   !state.is_range() &&
		   state.value == 0 &&
		   state.min_value == 0 &&
		   state.max_value == 0 &&
		   set_request.setting == cvmmap::CameraControlSetting::Unknown &&
		   set_request.mode == cvmmap::CameraControlWriteMode::Manual &&
		   set_request.value == 0 &&
		   set_range_request.setting == cvmmap::CameraControlSetting::Unknown &&
		   set_range_request.min_value == 0 &&
		   set_range_request.max_value == 0;
}


bool test_camera_control_types_hold_expected_values() {
	cvmmap::CameraControlCapabilities capabilities{};
	capabilities.supported = true;
	capabilities.supported_settings = {
		cvmmap::CameraControlSetting::Exposure,
		cvmmap::CameraControlSetting::AutoExposureTimeRange,
	};

	cvmmap::CameraControlState single_state{};
	single_state.setting = cvmmap::CameraControlSetting::Exposure;
	single_state.kind = cvmmap::CameraControlValueKind::Single;
	single_state.value = 42;

	cvmmap::CameraControlState range_state{};
	range_state.setting = cvmmap::CameraControlSetting::AutoExposureTimeRange;
	range_state.kind = cvmmap::CameraControlValueKind::Range;
	range_state.min_value = 2000;
	range_state.max_value = 5000;

	cvmmap::CameraControlRequest set_request{};
	set_request.setting = cvmmap::CameraControlSetting::Exposure;
	set_request.mode = cvmmap::CameraControlWriteMode::Manual;
	set_request.value = 55;

	cvmmap::CameraControlRangeRequest set_range_request{};
	set_range_request.setting = cvmmap::CameraControlSetting::AutoExposureTimeRange;
	set_range_request.min_value = 1500;
	set_range_request.max_value = 6000;

	return capabilities.supported &&
		   capabilities.supported_settings.size() == 2 &&
		   single_state.is_single() &&
		   !single_state.is_range() &&
		   single_state.value == 42 &&
		   range_state.is_range() &&
		   !range_state.is_single() &&
		   range_state.min_value == 2000 &&
		   range_state.max_value == 5000 &&
		   set_request.mode == cvmmap::CameraControlWriteMode::Manual &&
		   set_request.value == 55 &&
		   set_range_request.min_value == 1500 &&
		   set_range_request.max_value == 6000;
}

bool test_camera_control_nats_subjects() {
	return cvmmap::nats::subject_producer_camera_control_capabilities("demo") ==
			   "cvmmap.demo.producer.camera_control.capabilities" &&
		   cvmmap::nats::subject_producer_camera_control_get("demo") ==
			   "cvmmap.demo.producer.camera_control.get" &&
		   cvmmap::nats::subject_producer_camera_control_set("demo") ==
			   "cvmmap.demo.producer.camera_control.set" &&
		   cvmmap::nats::subject_producer_camera_control_set_range("demo") ==
			   "cvmmap.demo.producer.camera_control.set_range";
}

bool test_playlist_types_hold_expected_values() {
	cvmmap::PlaylistRequest request{};
	request.paths = {"/tmp/a.mcap", "/tmp/b.mcap"};
	request.sort_by_recording_time = true;

	cvmmap::PlaylistInfo info{};
	info.has_playlist = true;
	info.paths = request.paths;
	info.sort_by_recording_time = request.sort_by_recording_time;
	info.current_index = 1;
	info.current_path = request.paths[1];

	return request.paths.size() == 2 &&
		   request.sort_by_recording_time &&
		   info.has_playlist &&
		   info.paths.size() == 2 &&
		   info.sort_by_recording_time &&
		   info.current_index == 1 &&
		   info.current_path == "/tmp/b.mcap";
}

bool test_playlist_nats_subjects() {
	const auto apply_subject = cvmmap::nats::subject_producer_source_playlist_apply("demo");
	const auto info_subject = cvmmap::nats::subject_producer_source_playlist_info("demo");
	return apply_subject == "cvmmap.demo.producer.source.playlist.apply" &&
		   info_subject == "cvmmap.demo.producer.source.playlist.info";
}

} // namespace

int main() {
	if (!test_control_error_code_values()) {
		std::cerr << "control error code value test failed\n";
		return 1;
	}
	if (!test_module_status_values()) {
		std::cerr << "module status value test failed\n";
		return 1;
	}
	if (!test_control_error_defaults()) {
		std::cerr << "control error default test failed\n";
		return 1;
	}
	if (!test_camera_control_enum_values()) {
		std::cerr << "camera control enum value test failed\n";
		return 1;
	}
	if (!test_camera_control_type_defaults()) {
		std::cerr << "camera control default type test failed\n";
		return 1;
	}


	if (!test_camera_control_types_hold_expected_values()) {
		std::cerr << "camera control type test failed\n";
		return 1;
	}
	if (!test_camera_control_nats_subjects()) {
		std::cerr << "camera control NATS subject test failed\n";
		return 1;
	}
	if (!test_playlist_types_hold_expected_values()) {
		std::cerr << "playlist client types test failed\n";
		return 1;
	}
	if (!test_playlist_nats_subjects()) {
		std::cerr << "playlist NATS subject test failed\n";
		return 1;
	}
	return 0;
}
