#include <cvmmap/client.hpp>
#include <cvmmap/ipc.hpp>
#include <cvmmap/nats_subjects.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

bool test_control_header_offsets() {
	constexpr size_t request_header_size =
		offsetof(cvmmap::control_message_request_t, request_message_length) +
		sizeof(uint16_t);
	constexpr size_t response_header_size =
		offsetof(cvmmap::control_message_response_t, response_message_length) +
		sizeof(uint16_t);

	return sizeof(cvmmap::control_message_request_t) == 36 &&
		   sizeof(cvmmap::control_message_response_t) == 40 &&
		   offsetof(cvmmap::control_message_request_t, request_message_length) == 32 &&
		   offsetof(cvmmap::control_message_response_t, response_message_length) == 36 &&
		   request_header_size == 34 &&
		   response_header_size == 38;
}

bool test_source_info_wire_roundtrip() {
	cvmmap::source_info_response_v1_t wire{};
	wire.source_kind = cvmmap::SourceKind::Finite;
	wire.timestamp_domain = cvmmap::TimestampDomain::UnixEpochNs;
	wire.flags = cvmmap::SOURCE_INFO_FLAG_CAN_SEEK |
				 cvmmap::SOURCE_INFO_FLAG_HAS_DEPTH |
				 cvmmap::SOURCE_INFO_FLAG_HAS_BODY;
	wire.timeline_start_ns = 100;
	wire.timeline_end_ns = 250;
	wire.duration_ns = 150;
	wire.current_timestamp_ns = 175;
	wire.current_frame_count = 9;

	std::vector<uint8_t> bytes(sizeof(wire));
	std::memcpy(bytes.data(), &wire, sizeof(wire));

	cvmmap::source_info_response_v1_t parsed{};
	std::memcpy(&parsed, bytes.data(), sizeof(parsed));
	return parsed.struct_size == sizeof(cvmmap::source_info_response_v1_t) &&
		   parsed.source_kind == cvmmap::SourceKind::Finite &&
		   parsed.timestamp_domain == cvmmap::TimestampDomain::UnixEpochNs &&
		   parsed.flags == wire.flags &&
		   parsed.timeline_start_ns == 100 &&
		   parsed.timeline_end_ns == 250 &&
		   parsed.duration_ns == 150 &&
		   parsed.current_timestamp_ns == 175 &&
		   parsed.current_frame_count == 9;
}

bool test_seek_wire_roundtrip() {
	cvmmap::seek_timestamp_request_v1_t request{};
	request.target_timestamp_ns = 123456789ull;

	std::vector<uint8_t> request_bytes(sizeof(request));
	std::memcpy(request_bytes.data(), &request, sizeof(request));

	cvmmap::seek_timestamp_request_v1_t parsed_request{};
	std::memcpy(&parsed_request, request_bytes.data(), sizeof(parsed_request));
	if (parsed_request.struct_size != sizeof(cvmmap::seek_timestamp_request_v1_t) ||
		parsed_request.target_timestamp_ns != request.target_timestamp_ns) {
		return false;
	}

	cvmmap::seek_timestamp_response_v1_t response{};
	response.exact_match = 1;
	response.requested_timestamp_ns = request.target_timestamp_ns;
	response.landed_timestamp_ns = 123456999ull;
	response.landed_frame_count = 0;

	std::vector<uint8_t> response_bytes(sizeof(response));
	std::memcpy(response_bytes.data(), &response, sizeof(response));

	cvmmap::seek_timestamp_response_v1_t parsed_response{};
	std::memcpy(&parsed_response, response_bytes.data(), sizeof(parsed_response));
	return parsed_response.struct_size == sizeof(cvmmap::seek_timestamp_response_v1_t) &&
		   parsed_response.exact_match == 1 &&
		   parsed_response.requested_timestamp_ns == request.target_timestamp_ns &&
		   parsed_response.landed_timestamp_ns == 123456999ull &&
		   parsed_response.landed_frame_count == 0;
}

bool test_recording_start_request_roundtrip() {
	cvmmap::recording_start_request_v1_t request{};
	request.path_length = 17;

	std::vector<uint8_t> bytes(sizeof(request));
	std::memcpy(bytes.data(), &request, sizeof(request));

	cvmmap::recording_start_request_v1_t parsed{};
	std::memcpy(&parsed, bytes.data(), sizeof(parsed));
	return parsed.struct_size == sizeof(cvmmap::recording_start_request_v1_t) &&
		   parsed.flags == 0 &&
		   parsed.path_length == 17;
}

bool test_recording_status_response_roundtrip() {
	cvmmap::recording_status_response_v1_t response{};
	response.recording_format = cvmmap::RecordingFormat::Svo;
	response.flags = cvmmap::RECORDING_STATUS_FLAG_CAN_RECORD |
					 cvmmap::RECORDING_STATUS_FLAG_IS_RECORDING |
					 cvmmap::RECORDING_STATUS_FLAG_LAST_FRAME_OK;
	response.path_length = 17;
	response.frames_ingested = 42;
	response.frames_encoded = 40;

	std::vector<uint8_t> bytes(sizeof(response));
	std::memcpy(bytes.data(), &response, sizeof(response));

	cvmmap::recording_status_response_v1_t parsed{};
	std::memcpy(&parsed, bytes.data(), sizeof(parsed));
	return parsed.struct_size == sizeof(cvmmap::recording_status_response_v1_t) &&
		   parsed.recording_format == cvmmap::RecordingFormat::Svo &&
		   parsed.flags == response.flags &&
		   parsed.path_length == 17 &&
		   parsed.frames_ingested == 42 &&
		   parsed.frames_encoded == 40;
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
	const auto apply_subject = cvmmap::nats::subject_control_source_playlist_apply("demo");
	const auto info_subject = cvmmap::nats::subject_control_source_playlist_info("demo");
	return apply_subject == "cvmmap.demo.control.source.playlist.apply" &&
		   info_subject == "cvmmap.demo.control.source.playlist.info";
}

} // namespace

int main() {
	if (!test_control_header_offsets()) {
		std::cerr << "control header offset test failed\n";
		return 1;
	}
	if (!test_source_info_wire_roundtrip()) {
		std::cerr << "source info wire round-trip failed\n";
		return 1;
	}
	if (!test_seek_wire_roundtrip()) {
		std::cerr << "seek wire round-trip failed\n";
		return 1;
	}
	if (!test_recording_start_request_roundtrip()) {
		std::cerr << "recording start request round-trip failed\n";
		return 1;
	}
	if (!test_recording_status_response_roundtrip()) {
		std::cerr << "recording status response round-trip failed\n";
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
