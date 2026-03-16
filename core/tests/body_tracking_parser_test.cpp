#include <cvmmap/parser.hpp>
#include <cvmmap/target.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr auto kLabel = "example";

std::vector<uint8_t> make_valid_body_message() {
	cvmmap::body_tracking_message_header_t header{};
	header._magic              = cvmmap::BODY_TRACKING_MAGIC;
	header.versions_major      = cvmmap::VERSION_MAJOR;
	header.versions_minor      = cvmmap::VERSION_MINOR;
	header.frame_count         = 42;
	header.timestamp_ns        = 1000;
	header.sdk_timestamp_ns    = 2000;
	header.body_count          = 1;
	header.body_record_size    = sizeof(cvmmap::body_tracking_body_t);
	header.body_format         = cvmmap::BodyFormat::Body18;
	header.body_selection      = cvmmap::BodyKeypointSelection::Full;
	header.detection_model     = cvmmap::BodyTrackingModel::HumanBodyAccurate;
	header.inference_precision = cvmmap::InferencePrecision::FP32;
	header.flags               = cvmmap::BODY_TRACKING_FLAG_IS_NEW;
	header.set_coordinate_system(cvmmap::BodyCoordinateSystem::RightHandedYUp);
	header.set_reference_frame(cvmmap::BodyReferenceFrame::World);
	header.set_floor_as_origin(true);
	header.payload_size_bytes  = sizeof(cvmmap::body_tracking_body_t);
	std::memcpy(header._label, kLabel, std::strlen(kLabel));

	cvmmap::body_tracking_body_t body{};
	body.id             = 7;
	body.tracking_state = cvmmap::ObjectTrackingState::Ok;
	body.action_state   = cvmmap::ObjectActionState::Idle;
	body.confidence     = 88.5f;
	body.position       = {1.0f, 2.0f, 3.0f};
	body.velocity       = {4.0f, 5.0f, 6.0f};
	body.keypoint_count = 1;
	body.flags          = cvmmap::BODY_TRACKING_BODY_FLAG_HAS_ROOT_ORIENTATION;

	for (auto &point : body.bounding_box_2d) {
		point = {std::numeric_limits<float>::quiet_NaN(),
				 std::numeric_limits<float>::quiet_NaN()};
	}
	body.bounding_box_2d[0] = {10.0f, 11.0f};
	body.keypoint_2d[0]     = {12.0f, 13.0f};
	body.keypoint_3d[0]     = {1.0f, 2.0f, 3.0f};
	body.keypoint_confidence[0] = 0.9f;

	std::vector<uint8_t> bytes(
		sizeof(cvmmap::body_tracking_message_header_t) +
		sizeof(cvmmap::body_tracking_body_t));
	std::memcpy(bytes.data(), &header, sizeof(header));
	std::memcpy(
		bytes.data() + sizeof(cvmmap::body_tracking_message_header_t),
		&body,
		sizeof(body));
	return bytes;
}

bool test_target_resolution() {
	const auto resolved = cvmmap::resolve_cvmmap_target_or_throw(
		"cvmmap://camera0@/run/cvmmap?namespace=zed");
	return resolved.nats_target_key == "zed_camera0";
}

bool test_valid_body_parse() {
	const auto message = make_valid_body_message();
	auto parsed = cvmmap::parse_body_tracking_message(message);
	if (!parsed) {
		std::cerr << "expected valid body packet, got error: " << parsed.error()
				  << '\n';
		return false;
	}

	return parsed->header.frame_count == 42 &&
		   parsed->header.body_count == 1 &&
		   parsed->header.body_record_size ==
			   sizeof(cvmmap::body_tracking_body_t) &&
		   parsed->header.coordinate_system() ==
			   cvmmap::BodyCoordinateSystem::RightHandedYUp &&
		   parsed->header.reference_frame() ==
			   cvmmap::BodyReferenceFrame::World &&
		   parsed->header.floor_as_origin() &&
		   parsed->header.label() == kLabel &&
		   parsed->bodies.size() == 1 &&
		   parsed->bodies.front().id == 7 &&
		   std::fabs(parsed->bodies.front().position[0] - 1.0f) < 1e-6f &&
		   parsed->bodies.front().keypoint_count == 1;
}

bool test_invalid_record_size_rejected() {
	auto message = make_valid_body_message();
	auto *header =
		reinterpret_cast<cvmmap::body_tracking_message_header_t *>(message.data());
	header->body_record_size = 12;
	header->payload_size_bytes = 12;
	message.resize(sizeof(cvmmap::body_tracking_message_header_t) + 12);

	const auto parsed = cvmmap::parse_body_tracking_message(message);
	return !parsed &&
		   parsed.error().find("invalid body_record_size") !=
			   std::string::npos;
}

bool test_invalid_coordinate_system_rejected() {
	auto message = make_valid_body_message();
	auto *header =
		reinterpret_cast<cvmmap::body_tracking_message_header_t *>(message.data());
	header->set_coordinate_system(
		static_cast<cvmmap::BodyCoordinateSystem>(99));

	const auto parsed = cvmmap::parse_body_tracking_message(message);
	return !parsed &&
		   parsed.error().find("unsupported coordinate_system") !=
			   std::string::npos;
}

bool test_invalid_reference_frame_rejected() {
	auto message = make_valid_body_message();
	auto *header =
		reinterpret_cast<cvmmap::body_tracking_message_header_t *>(message.data());
	header->set_reference_frame(
		static_cast<cvmmap::BodyReferenceFrame>(99));

	const auto parsed = cvmmap::parse_body_tracking_message(message);
	return !parsed &&
		   parsed.error().find("unsupported reference_frame") !=
			   std::string::npos;
}

} // namespace

int main() {
	if (!test_target_resolution()) {
		std::cerr << "target resolution test failed\n";
		return 1;
	}
	if (!test_valid_body_parse()) {
		std::cerr << "valid body parse test failed\n";
		return 1;
	}
	if (!test_invalid_record_size_rejected()) {
		std::cerr << "invalid body record size test failed\n";
		return 1;
	}
	if (!test_invalid_coordinate_system_rejected()) {
		std::cerr << "invalid coordinate system test failed\n";
		return 1;
	}
	if (!test_invalid_reference_frame_rejected()) {
		std::cerr << "invalid reference frame test failed\n";
		return 1;
	}
	return 0;
}
