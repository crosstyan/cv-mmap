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
