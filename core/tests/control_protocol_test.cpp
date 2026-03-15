#include <cvmmap/ipc.hpp>

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
	return 0;
}
