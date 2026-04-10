#include <cvmmap/ipc.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

using byte_vector = std::vector<uint8_t>;

struct generated_file_t {
	std::string relative_path;
	byte_vector bytes;
};

constexpr std::string_view kSyncLabel = "camera_0";
constexpr std::string_view kBodyLabel = "example";

template <typename T>
byte_vector copy_struct_bytes(const T &value) {
	byte_vector bytes(sizeof(T));
	std::memcpy(bytes.data(), &value, sizeof(T));
	return bytes;
}

void assign_label(uint8_t (&label)[cvmmap::LABEL_LEN_MAX], const std::string_view value) {
	std::fill(std::begin(label), std::end(label), '\0');
	const auto len = std::min(value.size(), static_cast<size_t>(cvmmap::LABEL_LEN_MAX - 1));
	std::memcpy(label, value.data(), len);
}

byte_vector make_sync_message() {
	cvmmap::sync_message_t sync{};
	sync.frame_count = 42;
	sync.timestamp_ns = 123456789ull;
	assign_label(sync._label, kSyncLabel);
	return copy_struct_bytes(sync);
}

byte_vector make_body_tracking_message() {
	cvmmap::body_tracking_message_header_t header{};
	header._magic = cvmmap::BODY_TRACKING_MAGIC;
	header.versions_major = cvmmap::VERSION_MAJOR;
	header.versions_minor = cvmmap::VERSION_MINOR;
	header.frame_count = 42;
	header.timestamp_ns = 1000ull;
	header.sdk_timestamp_ns = 2000ull;
	header.body_count = 1;
	header.body_record_size = sizeof(cvmmap::body_tracking_body_t);
	header.body_format = cvmmap::BodyFormat::Body18;
	header.body_selection = cvmmap::BodyKeypointSelection::Full;
	header.detection_model = cvmmap::BodyTrackingModel::HumanBodyAccurate;
	header.inference_precision = cvmmap::InferencePrecision::FP32;
	header.flags = cvmmap::BODY_TRACKING_FLAG_IS_NEW;
	header.set_coordinate_system(cvmmap::BodyCoordinateSystem::RightHandedYUp);
	header.set_reference_frame(cvmmap::BodyReferenceFrame::World);
	header.set_floor_as_origin(true);
	header.payload_size_bytes = sizeof(cvmmap::body_tracking_body_t);
	assign_label(header._label, kBodyLabel);

	cvmmap::body_tracking_body_t body{};
	body.id = 7;
	body.tracking_state = cvmmap::ObjectTrackingState::Ok;
	body.action_state = cvmmap::ObjectActionState::Idle;
	body.confidence = 88.5f;
	body.position = {1.0f, 2.0f, 3.0f};
	body.velocity = {4.0f, 5.0f, 6.0f};
	body.keypoint_count = 1;
	body.flags = cvmmap::BODY_TRACKING_BODY_FLAG_HAS_ROOT_ORIENTATION;

	for (auto &point : body.bounding_box_2d) {
		point = {std::numeric_limits<float>::quiet_NaN(),
				 std::numeric_limits<float>::quiet_NaN()};
	}
	body.bounding_box_2d[0] = {10.0f, 11.0f};
	body.keypoint_2d[0] = {12.0f, 13.0f};
	body.keypoint_3d[0] = {1.0f, 2.0f, 3.0f};
	body.keypoint_confidence[0] = 0.9f;

	byte_vector bytes(
		sizeof(cvmmap::body_tracking_message_header_t) +
		sizeof(cvmmap::body_tracking_body_t));
	std::memcpy(bytes.data(), &header, sizeof(header));
	std::memcpy(
		bytes.data() + sizeof(cvmmap::body_tracking_message_header_t),
		&body,
		sizeof(body));
	return bytes;
}

std::string build_manifest_json() {
	std::ostringstream out;
	out << "{\n";
	out << "  \"sync_valid\": {\n";
	out << "    \"file\": \"sync_valid.bin\",\n";
	out << "    \"size\": " << sizeof(cvmmap::sync_message_t) << ",\n";
	out << "    \"frame_count\": 42,\n";
	out << "    \"timestamp_ns\": 123456789,\n";
	out << "    \"label\": \"" << kSyncLabel << "\"\n";
	out << "  },\n";
	out << "  \"body_tracking_valid\": {\n";
	out << "    \"file\": \"body_tracking_valid.bin\",\n";
	out << "    \"size\": "
		<< (sizeof(cvmmap::body_tracking_message_header_t) +
			sizeof(cvmmap::body_tracking_body_t)) << ",\n";
	out << "    \"frame_count\": 42,\n";
	out << "    \"timestamp_ns\": 1000,\n";
	out << "    \"sdk_timestamp_ns\": 2000,\n";
	out << "    \"body_count\": 1,\n";
	out << "    \"body_format\": " << static_cast<int>(cvmmap::BodyFormat::Body18) << ",\n";
	out << "    \"body_selection\": " << static_cast<int>(cvmmap::BodyKeypointSelection::Full) << ",\n";
	out << "    \"detection_model\": " << static_cast<int>(cvmmap::BodyTrackingModel::HumanBodyAccurate) << ",\n";
	out << "    \"inference_precision\": " << static_cast<int>(cvmmap::InferencePrecision::FP32) << ",\n";
	out << "    \"flags\": "
		<< (cvmmap::BODY_TRACKING_FLAG_IS_NEW | cvmmap::BODY_TRACKING_FLAG_FLOOR_AS_ORIGIN) << ",\n";
	out << "    \"coordinate_system\": " << static_cast<int>(cvmmap::BodyCoordinateSystem::RightHandedYUp) << ",\n";
	out << "    \"reference_frame\": " << static_cast<int>(cvmmap::BodyReferenceFrame::World) << ",\n";
	out << "    \"floor_as_origin\": true,\n";
	out << "    \"label\": \"" << kBodyLabel << "\",\n";
	out << "    \"body\": {\n";
	out << "      \"id\": 7,\n";
	out << "      \"tracking_state\": " << static_cast<int>(cvmmap::ObjectTrackingState::Ok) << ",\n";
	out << "      \"action_state\": " << static_cast<int>(cvmmap::ObjectActionState::Idle) << ",\n";
	out << "      \"confidence\": 88.5,\n";
	out << "      \"position\": [1.0, 2.0, 3.0],\n";
	out << "      \"keypoint_count\": 1,\n";
	out << "      \"flags\": " << cvmmap::BODY_TRACKING_BODY_FLAG_HAS_ROOT_ORIENTATION << "\n";
	out << "    }\n";
	out << "  }\n";
	out << "}\n";
	return out.str();
}

std::vector<generated_file_t> build_generated_files() {
	const auto manifest = build_manifest_json();

	std::vector<generated_file_t> files;
	files.push_back({"sync_valid.bin", make_sync_message()});
	files.push_back({"body_tracking_valid.bin", make_body_tracking_message()});
	files.push_back({
		"manifest.json",
		byte_vector(manifest.begin(), manifest.end()),
	});
	return files;
}

bool verify_file(const fs::path &path, std::span<const uint8_t> expected) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		std::cerr << "missing fixture file: " << path << '\n';
		return false;
	}
	const auto actual = byte_vector(
		std::istreambuf_iterator<char>(in),
		std::istreambuf_iterator<char>());
	if (actual.size() != expected.size() ||
		!std::equal(actual.begin(), actual.end(), expected.begin(), expected.end())) {
		std::cerr << "fixture mismatch: " << path << '\n';
		return false;
	}
	return true;
}

bool write_file(const fs::path &path, std::span<const uint8_t> bytes) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out) {
		std::cerr << "failed to open fixture file for write: " << path << '\n';
		return false;
	}
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return out.good();
}

int run_verify(const fs::path &dir) {
	const auto files = build_generated_files();
	for (const auto &file : files) {
		if (!verify_file(dir / file.relative_path, file.bytes)) {
			return 1;
		}
	}
	return 0;
}

int run_write(const fs::path &dir) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		std::cerr << "failed to create fixture directory " << dir << ": " << ec.message() << '\n';
		return 1;
	}

	const auto files = build_generated_files();
	for (const auto &file : files) {
		if (!write_file(dir / file.relative_path, file.bytes)) {
			return 1;
		}
	}
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc != 3) {
		std::cerr << "usage: " << argv[0] << " --write <dir> | --verify <dir>\n";
		return 1;
	}

	const std::string_view mode = argv[1];
	const fs::path dir = argv[2];
	if (mode == "--write") {
		return run_write(dir);
	}
	if (mode == "--verify") {
		return run_verify(dir);
	}

	std::cerr << "unknown mode: " << mode << '\n';
	return 1;
}
