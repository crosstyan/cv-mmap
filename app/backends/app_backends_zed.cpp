#include <errno.h>
#include <memory>
#include <utility>

#if defined(WITH_BACKEND_ZED) && __has_include(<sl/Camera.hpp>)
#include <sl/Camera.hpp>
#define APP_HAS_ZED_SDK 1
#else
#define APP_HAS_ZED_SDK 0
#endif

#if APP_HAS_ZED_SDK
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <cvmmap/compat/format.hpp>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#endif

#include <cvmmap/compat/expected.hpp>
#include <spdlog/spdlog.h>

#include "app_backends_facade.hpp"
#include "app_backends_zed.hpp"
#if APP_HAS_ZED_SDK
#include "app_config.hpp"
#include "app_enum_models.hpp"
#endif

namespace app::backends {

#if APP_HAS_ZED_SDK

namespace {

std::string normalize_ascii_lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

bool is_network_stream_mode(const std::string_view mode) {
	const auto normalized = normalize_ascii_lower(std::string(mode));
	return normalized == "network" || normalized == "ethernet" || normalized == "stream";
}

bool is_svo_stream_mode(const std::string_view mode) {
	return normalize_ascii_lower(std::string(mode)) == "svo";
}

struct ZedCameraControlDescriptor {
	cvmmap::CameraControlSetting setting;
	cvmmap::CameraControlValueKind kind;
	sl::VIDEO_SETTINGS zed_setting;
	bool legacy_supported;
	bool x_family_supported;
};

constexpr std::array<ZedCameraControlDescriptor, 3> ZED_CAMERA_CONTROL_DESCRIPTORS{{
	{cvmmap::CameraControlSetting::Exposure, cvmmap::CameraControlValueKind::Single, sl::VIDEO_SETTINGS::EXPOSURE, true, false},
	{cvmmap::CameraControlSetting::ExposureTime, cvmmap::CameraControlValueKind::Single, sl::VIDEO_SETTINGS::EXPOSURE_TIME, false, true},
	{cvmmap::CameraControlSetting::AutoExposureTimeRange, cvmmap::CameraControlValueKind::Range, sl::VIDEO_SETTINGS::AUTO_EXPOSURE_TIME_RANGE, false, true},
}};

bool is_zed_x_family_model(const sl::MODEL model) {
	switch (model) {
	case sl::MODEL::ZED_X:
	case sl::MODEL::ZED_XM:
	case sl::MODEL::ZED_X_HDR:
	case sl::MODEL::ZED_X_HDR_MINI:
	case sl::MODEL::ZED_X_HDR_MAX:
	case sl::MODEL::VIRTUAL_ZED_X:
	case sl::MODEL::ZED_XONE_GS:
	case sl::MODEL::ZED_XONE_UHD:
	case sl::MODEL::ZED_XONE_HDR:
		return true;
	default:
		return false;
	}
}

const ZedCameraControlDescriptor *find_zed_camera_control_descriptor(const cvmmap::CameraControlSetting setting) {
	for (const auto &descriptor : ZED_CAMERA_CONTROL_DESCRIPTORS) {
		if (descriptor.setting == setting) {
			return &descriptor;
		}
	}
	return nullptr;
}

bool is_zed_camera_control_supported(const ZedCameraControlDescriptor &descriptor, const sl::MODEL model) {
	return is_zed_x_family_model(model) ? descriptor.x_family_supported : descriptor.legacy_supported;
}

sl::RESOLUTION parse_resolution(const std::string& value) {
	const auto normalized = normalize_ascii_lower(value);

	// Canonical ZED SDK values
	if (normalized == "hd2k") {
		return sl::RESOLUTION::HD2K;
	}
	if (normalized == "hd1200") {
		return sl::RESOLUTION::HD1200;
	}
	if (normalized == "hd1080") {
		return sl::RESOLUTION::HD1080;
	}
	if (normalized == "hd720") {
		return sl::RESOLUTION::HD720;
	}
	if (normalized == "svga") {
		return sl::RESOLUTION::SVGA;
	}
	if (normalized == "vga") {
		return sl::RESOLUTION::VGA;
	}
	if (normalized == "auto") {
		return sl::RESOLUTION::AUTO;
	}

	// Canonical aliases (common resolution names)
	if (normalized == "2k") {
		return sl::RESOLUTION::HD2K;
	}
	if (normalized == "1080p" || normalized == "fhd") {
		return sl::RESOLUTION::HD1080;
	}
	if (normalized == "720p" || normalized == "hd") {
		return sl::RESOLUTION::HD720;
	}

	throw std::invalid_argument("unknown ZED resolution: " + value + "; valid values: hd2k|hd1200|hd1080|hd720|svga|vga|auto or aliases: 2k|1080p|fhd|720p|hd");
}

sl::DEPTH_MODE parse_depth_mode(const std::string& value) {
	auto normalized = normalize_ascii_lower(value);
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');
	if (normalized == "none") {
		return sl::DEPTH_MODE::NONE;
	}
	if (normalized == "neural_light") {
		return sl::DEPTH_MODE::NEURAL_LIGHT;
	}
	if (normalized == "neural") {
		return sl::DEPTH_MODE::NEURAL;
	}
	if (normalized == "neural_plus") {
		return sl::DEPTH_MODE::NEURAL_PLUS;
	}

	throw std::invalid_argument(
		"unknown ZED depth_mode: " + value +
		"; valid values: none|neural|neural_light|neural_plus"
	);
}

sl::SVO_COMPRESSION_MODE parse_recording_compression_mode(const std::string &value) {
	auto normalized = normalize_ascii_lower(value);
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	if (normalized == "lossless") {
		return sl::SVO_COMPRESSION_MODE::LOSSLESS;
	}
	if (normalized == "h264") {
		return sl::SVO_COMPRESSION_MODE::H264;
	}
	if (normalized == "h265") {
		return sl::SVO_COMPRESSION_MODE::H265;
	}
	if (normalized == "h264_lossless") {
		return sl::SVO_COMPRESSION_MODE::H264_LOSSLESS;
	}
	if (normalized == "h265_lossless") {
		return sl::SVO_COMPRESSION_MODE::H265_LOSSLESS;
	}

	throw std::invalid_argument(
		"unknown ZED recording compression_mode: " + value +
		"; valid values: lossless|h264|h265|h264_lossless|h265_lossless"
	);
}

sl::VIEW parse_left_view(std::string value) {
	value = normalize_ascii_lower(std::move(value));
	if (value == "gray8" || value == "mono8") {
		return sl::VIEW::LEFT_GRAY;
	}
	if (value == "bgr8" || value == "rgb8") {
		return sl::VIEW::LEFT_BGR;
	}
	return sl::VIEW::LEFT_BGRA;
}

sl::BODY_TRACKING_MODEL parse_body_tracking_model(const std::string &value) {
	const auto normalized = normalize_ascii_lower(value);
	if (normalized == "human_body_fast") {
		return sl::BODY_TRACKING_MODEL::HUMAN_BODY_FAST;
	}
	if (normalized == "human_body_medium") {
		return sl::BODY_TRACKING_MODEL::HUMAN_BODY_MEDIUM;
	}
	return sl::BODY_TRACKING_MODEL::HUMAN_BODY_ACCURATE;
}

sl::BODY_FORMAT parse_body_format(const std::string &value) {
	const auto normalized = normalize_ascii_lower(value);
	if (normalized == "body_34") {
		return sl::BODY_FORMAT::BODY_34;
	}
	if (normalized == "body_38") {
		return sl::BODY_FORMAT::BODY_38;
	}
	return sl::BODY_FORMAT::BODY_18;
}

sl::BODY_KEYPOINTS_SELECTION parse_body_selection(const std::string &value) {
	const auto normalized = normalize_ascii_lower(value);
	if (normalized == "upper_body") {
		return sl::BODY_KEYPOINTS_SELECTION::UPPER_BODY;
	}
	return sl::BODY_KEYPOINTS_SELECTION::FULL;
}

sl::COORDINATE_SYSTEM parse_coordinate_system(const std::string &value) {
	auto normalized = normalize_ascii_lower(value);
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');
	if (normalized == "image") {
		return sl::COORDINATE_SYSTEM::IMAGE;
	}
	if (normalized == "right_handed_y_up") {
		return sl::COORDINATE_SYSTEM::RIGHT_HANDED_Y_UP;
	}
	throw std::invalid_argument(
		"unsupported ZED coordinate_system: " + value +
		"; supported values: IMAGE|RIGHT_HANDED_Y_UP");
}

sl::REFERENCE_FRAME parse_body_reference_frame(const std::string &value) {
	auto normalized = normalize_ascii_lower(value);
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');
	if (normalized == "camera") {
		return sl::REFERENCE_FRAME::CAMERA;
	}
	if (normalized == "world") {
		return sl::REFERENCE_FRAME::WORLD;
	}
	throw std::invalid_argument(
		"unsupported ZED reference_frame: " + value +
		"; supported values: CAMERA|WORLD");
}

cvmmap::BodyTrackingModel to_body_tracking_model(const sl::BODY_TRACKING_MODEL value) {
	switch (value) {
	case sl::BODY_TRACKING_MODEL::HUMAN_BODY_FAST:
		return cvmmap::BodyTrackingModel::HumanBodyFast;
	case sl::BODY_TRACKING_MODEL::HUMAN_BODY_MEDIUM:
		return cvmmap::BodyTrackingModel::HumanBodyMedium;
	case sl::BODY_TRACKING_MODEL::HUMAN_BODY_ACCURATE:
	default:
		return cvmmap::BodyTrackingModel::HumanBodyAccurate;
	}
}

cvmmap::BodyFormat to_body_format(const sl::BODY_FORMAT value) {
	switch (value) {
	case sl::BODY_FORMAT::BODY_34:
		return cvmmap::BodyFormat::Body34;
	case sl::BODY_FORMAT::BODY_38:
		return cvmmap::BodyFormat::Body38;
	case sl::BODY_FORMAT::BODY_18:
	default:
		return cvmmap::BodyFormat::Body18;
	}
}

cvmmap::BodyKeypointSelection to_body_selection(const sl::BODY_KEYPOINTS_SELECTION value) {
	switch (value) {
	case sl::BODY_KEYPOINTS_SELECTION::UPPER_BODY:
		return cvmmap::BodyKeypointSelection::UpperBody;
	case sl::BODY_KEYPOINTS_SELECTION::FULL:
	default:
		return cvmmap::BodyKeypointSelection::Full;
	}
}

cvmmap::BodyCoordinateSystem to_body_coordinate_system(
	const sl::COORDINATE_SYSTEM value) {
	switch (value) {
	case sl::COORDINATE_SYSTEM::IMAGE:
		return cvmmap::BodyCoordinateSystem::Image;
	case sl::COORDINATE_SYSTEM::RIGHT_HANDED_Y_UP:
		return cvmmap::BodyCoordinateSystem::RightHandedYUp;
	default:
		return cvmmap::BodyCoordinateSystem::Unknown;
	}
}

cvmmap::BodyReferenceFrame to_body_reference_frame(
	const sl::REFERENCE_FRAME value) {
	switch (value) {
	case sl::REFERENCE_FRAME::CAMERA:
		return cvmmap::BodyReferenceFrame::Camera;
	case sl::REFERENCE_FRAME::WORLD:
		return cvmmap::BodyReferenceFrame::World;
	default:
		return cvmmap::BodyReferenceFrame::Unknown;
	}
}

cvmmap::InferencePrecision to_inference_precision(const sl::INFERENCE_PRECISION value) {
	switch (value) {
	case sl::INFERENCE_PRECISION::FP16:
		return cvmmap::InferencePrecision::FP16;
	case sl::INFERENCE_PRECISION::INT8:
		return cvmmap::InferencePrecision::INT8;
	case sl::INFERENCE_PRECISION::FP32:
	default:
		return cvmmap::InferencePrecision::FP32;
	}
}

cvmmap::ObjectTrackingState to_tracking_state(const sl::OBJECT_TRACKING_STATE value) {
	switch (value) {
	case sl::OBJECT_TRACKING_STATE::OFF:
		return cvmmap::ObjectTrackingState::Off;
	case sl::OBJECT_TRACKING_STATE::OK:
		return cvmmap::ObjectTrackingState::Ok;
	case sl::OBJECT_TRACKING_STATE::SEARCHING:
		return cvmmap::ObjectTrackingState::Searching;
	case sl::OBJECT_TRACKING_STATE::TERMINATE:
	default:
		return cvmmap::ObjectTrackingState::Terminate;
	}
}

cvmmap::ObjectActionState to_action_state(const sl::OBJECT_ACTION_STATE value) {
	switch (value) {
	case sl::OBJECT_ACTION_STATE::IDLE:
		return cvmmap::ObjectActionState::Idle;
	case sl::OBJECT_ACTION_STATE::MOVING:
		return cvmmap::ObjectActionState::Moving;
	default:
		return cvmmap::ObjectActionState::Idle;
	}
}

float nan32() {
	return std::numeric_limits<float>::quiet_NaN();
}

template <size_t N>
void fill_nan(std::array<float, N> &out) {
	out.fill(nan32());
}

template <size_t N>
void copy_float3(std::array<float, N> &out, const sl::float3 &value) {
	static_assert(N == 3);
	out[0] = value.x;
	out[1] = value.y;
	out[2] = value.z;
}

template <size_t N>
void copy_float4(std::array<float, N> &out, const sl::float4 &value) {
	static_assert(N == 4);
	out[0] = value.x;
	out[1] = value.y;
	out[2] = value.z;
	out[3] = value.w;
}

void copy_uint2_box(std::array<std::array<float, 2>, cvmmap::BODY_BOX2D_POINTS> &dst, const std::vector<sl::uint2> &src) {
	for (size_t i = 0; i < cvmmap::BODY_BOX2D_POINTS; ++i) {
		dst[i][0] = nan32();
		dst[i][1] = nan32();
	}
	for (size_t i = 0; i < std::min(src.size(), static_cast<size_t>(cvmmap::BODY_BOX2D_POINTS)); ++i) {
		dst[i][0] = static_cast<float>(src[i].x);
		dst[i][1] = static_cast<float>(src[i].y);
	}
}

void copy_float3_box(std::array<std::array<float, 3>, cvmmap::BODY_BOX3D_POINTS> &dst, const std::vector<sl::float3> &src) {
	for (size_t i = 0; i < cvmmap::BODY_BOX3D_POINTS; ++i) {
		dst[i][0] = nan32();
		dst[i][1] = nan32();
		dst[i][2] = nan32();
	}
	for (size_t i = 0; i < std::min(src.size(), static_cast<size_t>(cvmmap::BODY_BOX3D_POINTS)); ++i) {
		dst[i][0] = src[i].x;
		dst[i][1] = src[i].y;
		dst[i][2] = src[i].z;
	}
}

void initialize_body_record(cvmmap::body_tracking_body_t &record) {
	std::memset(&record, 0, sizeof(record));
	record.id = -1;
	record.confidence = nan32();
	fill_nan(record.position);
	fill_nan(record.velocity);
	fill_nan(record.position_covariance);
	fill_nan(record.dimensions);
	fill_nan(record.head_position);
	fill_nan(record.global_root_orientation);
	for (size_t i = 0; i < cvmmap::BODY_KEYPOINT_CAPACITY; ++i) {
		record.keypoint_2d[i][0] = nan32();
		record.keypoint_2d[i][1] = nan32();
		record.keypoint_3d[i][0] = nan32();
		record.keypoint_3d[i][1] = nan32();
		record.keypoint_3d[i][2] = nan32();
		record.keypoint_confidence[i] = nan32();
		for (size_t j = 0; j < 6; ++j) {
			record.keypoint_covariance[i][j] = nan32();
		}
		record.local_position_per_joint[i][0] = nan32();
		record.local_position_per_joint[i][1] = nan32();
		record.local_position_per_joint[i][2] = nan32();
		record.local_orientation_per_joint[i][0] = nan32();
		record.local_orientation_per_joint[i][1] = nan32();
		record.local_orientation_per_joint[i][2] = nan32();
		record.local_orientation_per_joint[i][3] = nan32();
	}
	copy_uint2_box(record.bounding_box_2d, {});
	copy_float3_box(record.bounding_box_3d, {});
	copy_uint2_box(record.head_bounding_box_2d, {});
	copy_float3_box(record.head_bounding_box_3d, {});
}

void copy_body_record(cvmmap::body_tracking_body_t &record, const sl::BodyData &body) {
	initialize_body_record(record);
	record.id = body.id;
	record.tracking_state = to_tracking_state(body.tracking_state);
	record.action_state = to_action_state(body.action_state);
	record.confidence = body.confidence;
	copy_float3(record.position, body.position);
	copy_float3(record.velocity, body.velocity);
	for (size_t i = 0; i < 6; ++i) {
		record.position_covariance[i] = body.position_covariance[i];
	}
	copy_uint2_box(record.bounding_box_2d, body.bounding_box_2d);
	copy_float3_box(record.bounding_box_3d, body.bounding_box);
	copy_float3(record.dimensions, body.dimensions);
	copy_uint2_box(record.head_bounding_box_2d, body.head_bounding_box_2d);
	copy_float3_box(record.head_bounding_box_3d, body.head_bounding_box);
	copy_float3(record.head_position, body.head_position);

	const auto keypoint_count =
		std::min(body.keypoint.size(), static_cast<size_t>(cvmmap::BODY_KEYPOINT_CAPACITY));
	record.keypoint_count = static_cast<uint16_t>(keypoint_count);

	for (size_t i = 0; i < std::min(body.keypoint_2d.size(), static_cast<size_t>(cvmmap::BODY_KEYPOINT_CAPACITY)); ++i) {
		record.keypoint_2d[i][0] = body.keypoint_2d[i].x;
		record.keypoint_2d[i][1] = body.keypoint_2d[i].y;
	}
	for (size_t i = 0; i < keypoint_count; ++i) {
		record.keypoint_3d[i][0] = body.keypoint[i].x;
		record.keypoint_3d[i][1] = body.keypoint[i].y;
		record.keypoint_3d[i][2] = body.keypoint[i].z;
	}
	for (size_t i = 0; i < std::min(body.keypoint_confidence.size(), static_cast<size_t>(cvmmap::BODY_KEYPOINT_CAPACITY)); ++i) {
		record.keypoint_confidence[i] = body.keypoint_confidence[i];
	}
	for (size_t i = 0; i < std::min(body.keypoint_covariances.size(), static_cast<size_t>(cvmmap::BODY_KEYPOINT_CAPACITY)); ++i) {
		for (size_t j = 0; j < 6; ++j) {
			record.keypoint_covariance[i][j] = body.keypoint_covariances[i][j];
		}
	}
	for (size_t i = 0; i < std::min(body.local_position_per_joint.size(), static_cast<size_t>(cvmmap::BODY_KEYPOINT_CAPACITY)); ++i) {
		record.local_position_per_joint[i][0] = body.local_position_per_joint[i].x;
		record.local_position_per_joint[i][1] = body.local_position_per_joint[i].y;
		record.local_position_per_joint[i][2] = body.local_position_per_joint[i].z;
	}
	if (!body.local_position_per_joint.empty()) {
		record.flags |= cvmmap::BODY_TRACKING_BODY_FLAG_HAS_LOCAL_JOINTS;
	}
	for (size_t i = 0; i < std::min(body.local_orientation_per_joint.size(), static_cast<size_t>(cvmmap::BODY_KEYPOINT_CAPACITY)); ++i) {
		record.local_orientation_per_joint[i][0] = body.local_orientation_per_joint[i].x;
		record.local_orientation_per_joint[i][1] = body.local_orientation_per_joint[i].y;
		record.local_orientation_per_joint[i][2] = body.local_orientation_per_joint[i].z;
		record.local_orientation_per_joint[i][3] = body.local_orientation_per_joint[i].w;
	}
	if (!body.local_orientation_per_joint.empty()) {
		record.flags |= cvmmap::BODY_TRACKING_BODY_FLAG_HAS_LOCAL_JOINTS;
	}
	if (std::isfinite(body.global_root_orientation.x) &&
		std::isfinite(body.global_root_orientation.y) &&
		std::isfinite(body.global_root_orientation.z) &&
		std::isfinite(body.global_root_orientation.w)) {
		copy_float4(record.global_root_orientation, body.global_root_orientation);
		record.flags |= cvmmap::BODY_TRACKING_BODY_FLAG_HAS_ROOT_ORIENTATION;
	}
}

PixelFormat guess_pixel_format_for_zed(const sl::MAT_TYPE mat_type) {
	switch (mat_type) {
	case sl::MAT_TYPE::U8_C1:
		return PixelFormat::GRAY;
	case sl::MAT_TYPE::U8_C2:
		return PixelFormat::YUYV;
	case sl::MAT_TYPE::U8_C3:
		return PixelFormat::BGR;
	case sl::MAT_TYPE::U8_C4:
		return PixelFormat::BGRA;
	default:
		return PixelFormat::BGR;
	}
}

uint8_t channels_from_mat_type(const sl::MAT_TYPE mat_type) {
	switch (mat_type) {
	case sl::MAT_TYPE::U8_C1:
		return 1;
	case sl::MAT_TYPE::U8_C2:
		return 2;
	case sl::MAT_TYPE::U8_C3:
		return 3;
	case sl::MAT_TYPE::U8_C4:
		return 4;
	case sl::MAT_TYPE::F32_C1:
		return 1;
	case sl::MAT_TYPE::U16_C1:
		return 1;
	default:
		return 0;
	}
}

std::optional<Depth> depth_from_mat_type(const sl::MAT_TYPE mat_type) {
	switch (mat_type) {
	case sl::MAT_TYPE::U8_C1:
	case sl::MAT_TYPE::U8_C2:
	case sl::MAT_TYPE::U8_C3:
	case sl::MAT_TYPE::U8_C4:
		return Depth::U8;
	case sl::MAT_TYPE::U16_C1:
		return Depth::U16;
	case sl::MAT_TYPE::F32_C1:
		return Depth::F32;
	default:
		return std::nullopt;
	}
}

uint64_t now_ns() {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

uint64_t zed_image_timestamp_ns(sl::Camera &camera) {
	return static_cast<uint64_t>(
		camera.getTimestamp(sl::TIME_REFERENCE::IMAGE).getNanoseconds());
}

cvmmap::expected<uint64_t, std::string> probe_zed_svo_start_timestamp_ns(
	const std::string &path) {
	if (path.empty()) {
		return cvmmap::unexpected("ZED SVO path is empty");
	}

	sl::Camera camera{};
	sl::InitParameters init_parameters{};
	init_parameters.depth_mode = sl::DEPTH_MODE::NONE;
	init_parameters.svo_real_time_mode = true;
	init_parameters.input.setFromSVOFile(sl::String(path.c_str()));

	const auto open_result = camera.open(init_parameters);
	if (open_result != sl::ERROR_CODE::SUCCESS) {
		return cvmmap::unexpected(cvmmap::format(
			"open ZED SVO failed: {}",
			sl::toString(open_result).get()));
	}

	const auto frame_count = camera.getSVONumberOfFrames();
	if (frame_count <= 0) {
		camera.close();
		return cvmmap::unexpected("ZED SVO file does not contain any frames");
	}

	camera.setSVOPosition(0);
	const auto grab_result = camera.grab();
	if (grab_result != sl::ERROR_CODE::SUCCESS) {
		camera.close();
		return cvmmap::unexpected(cvmmap::format(
			"initial ZED SVO grab failed: {}",
			sl::toString(grab_result).get()));
	}

	const auto timestamp_ns = zed_image_timestamp_ns(camera);
	camera.close();
	if (timestamp_ns == 0) {
		return cvmmap::unexpected("ZED SVO file returned zero image timestamp");
	}
	return timestamp_ns;
}

}

struct ZedBackendOptions {
	app::ZedConfig zed_config;
	app::VideoConfig video_config;
};

struct ZedBackendImpl {
	struct CapturedFrame {
		frame_info_t info{};
		source_info_t source_info{};
		std::vector<uint8_t> payload{};
		std::optional<cvmmap::body_tracking_frame_t> body_tracking{};
		uint64_t timestamp_ns{0};
	};

	struct PublishedSnapshot {
		frame_metadata_t metadata{};
		source_info_t source_info{};
		std::string last_recording_error{};
		std::chrono::steady_clock::time_point last_publish_at{};
		bool has_last_publish_at{false};
	};

	struct PublishedFrame {
		frame_metadata_t metadata{};
		std::vector<uint8_t> payload{};
		std::optional<cvmmap::body_tracking_frame_t> body_tracking{};
	};

	struct DirectPublishedFrame {
		frame_metadata_t metadata{};
		zed_frame_fill_fn_t fill_payload{};
		std::optional<cvmmap::body_tracking_frame_t> body_tracking{};
	};

	ZedBackendOptions options;
	sl::Camera camera;
	sl::InitParameters init_parameters{};
	sl::RuntimeParameters runtime_parameters{};
	sl::BodyTrackingParameters body_tracking_parameters{};
	sl::BodyTrackingRuntimeParameters body_tracking_runtime_parameters{};
	sl::Bodies bodies{};
	sl::PositionalTrackingParameters positional_tracking_parameters{};
	bool depth_enabled{false};
	bool body_tracking_enabled{false};
	sl::VIEW left_view{sl::VIEW::LEFT};
	sl::Mat left_frame;
	sl::Mat depth_frame;
	sl::Mat confidence_frame;
	std::vector<uint8_t> last_good_depth_plane;
	sl::REFERENCE_FRAME body_reference_frame{sl::REFERENCE_FRAME::CAMERA};
	std::string active_recording_path{};
	bool svo_mode{false};
	int total_svo_frames{0};
	uint64_t timeline_start_ns{0};
	uint64_t timeline_end_ns{0};
	std::jthread worker_thread;
	std::atomic<bool> initialized{false};
	mutable std::mutex camera_mutex{};
	mutable std::mutex snapshot_mutex{};

	on_metadata_fn_t _on_metadata{nullptr};
	 on_frame_fn_t _on_frame{nullptr};
	 on_zed_frame_direct_fn_t _on_frame_direct{nullptr};
	 on_body_tracking_fn_t _on_body_tracking{nullptr};
	 on_error_fn_t _on_error{nullptr};

	PublishedSnapshot published{};

	// =======================================================================
	// TASK 10 EXTENSION POINTS: Ethernet-ready placeholder stubs
	// =======================================================================
	// These placeholders document where future ethernet sender/receiver
	// orchestration logic will be integrated. They are non-functional today
	// and preserve all existing runtime semantics.
	//
	// Future work (not yet implemented):
	// - StreamingSender lifecycle management for sender mode
	// - StreamingReceiver coordination for advanced receiver features
	// - Sender discovery and connection health monitoring
	// - Dynamic stream quality adaptation
	//
	// See: /usr/local/zed/samples/camera streaming/ for SDK patterns

	struct EthernetExtensionStubs {
		// TODO: sl::StreamingSender sender;      // For sender mode (future)
		// TODO: sl::StreamingReceiver receiver;  // For receiver coordination (future)
		// TODO: std::jthread discovery_thread;   // For sender discovery (future)
		// TODO: std::atomic<bool> sender_ready{false}; // Sender health flag (future)

		// Extension: Log placeholder activation for visibility
		void log_placeholder_status() const {
			spdlog::debug("ZED ethernet extension stubs active (non-functional placeholders)");
		}
	} eth_stubs;
	// =======================================================================

	ZedBackendImpl() = default;
	explicit ZedBackendImpl(ZedBackendOptions opts) : options(std::move(opts)) {}

	~ZedBackendImpl() {
		Shutdown();
	}

	void on_metadata(const frame_metadata_t &m) {
		if (_on_metadata) {
			_on_metadata(m);
		}
	}

	void on_frame(std::span<uint8_t> frame_buffer, const frame_metadata_t &m) {
		if (_on_frame) {
			_on_frame(frame_buffer, m);
		}
	}

	void on_frame_direct(ZedDirectFrame frame) {
		if (_on_frame_direct) {
			_on_frame_direct(std::move(frame));
		}
	}

	void on_error(error_t error_code, std::string_view message) {
		if (_on_error) {
			_on_error(error_code, message);
		}
	}

	void on_body_tracking(const cvmmap::body_tracking_frame_t &frame) {
		if (_on_body_tracking) {
			_on_body_tracking(frame);
		}
	}

	void clear_recording_error() {
		std::lock_guard lock(snapshot_mutex);
		published.last_recording_error.clear();
	}

	void set_recording_error(std::string message) {
		std::lock_guard lock(snapshot_mutex);
		published.last_recording_error = std::move(message);
	}

	std::string GetLastRecordingError() {
		std::lock_guard lock(snapshot_mutex);
		return published.last_recording_error;
	}

 	error_t camera_control_availability_error_locked() const {
 		if (svo_mode) {
 			return -EOPNOTSUPP;
 		}
 		if (!initialized.load(std::memory_order_relaxed) || !camera.isOpened()) {
 			return -ENODEV;
 		}
 		return ERR_OK;
 	}

 	static error_t map_camera_control_operation_error(
 		sl::ERROR_CODE code,
 		error_t invalid_parameters_error) {
 		switch (code) {
 		case sl::ERROR_CODE::SUCCESS:
 			return ERR_OK;
 		case sl::ERROR_CODE::CAMERA_NOT_DETECTED:
 			return -ENODEV;
 		case sl::ERROR_CODE::INVALID_FUNCTION_CALL:
 			return -EOPNOTSUPP;
 		case sl::ERROR_CODE::INVALID_FUNCTION_PARAMETERS:
 			return invalid_parameters_error;
 		default:
 			return -EIO;
 		}
 	}

 	bool can_read_camera_control_locked(const ZedCameraControlDescriptor &descriptor) {
 		if (descriptor.kind == cvmmap::CameraControlValueKind::Single) {
 			int value = 0;
 			return camera.getCameraSettings(descriptor.zed_setting, value) == sl::ERROR_CODE::SUCCESS;
 		}

 		int min_value = 0;
 		int max_value = 0;
 		return camera.getCameraSettings(descriptor.zed_setting, min_value, max_value) == sl::ERROR_CODE::SUCCESS;
 	}

 	cvmmap::expected<camera_control_state_t, error_t> get_camera_control_locked(
 		const ZedCameraControlDescriptor &descriptor) {
 		camera_control_state_t state{};
 		state.setting = descriptor.setting;
 		state.kind = descriptor.kind;

 		if (descriptor.kind == cvmmap::CameraControlValueKind::Single) {
 			int value = 0;
 			const auto result = camera.getCameraSettings(descriptor.zed_setting, value);
 			if (result != sl::ERROR_CODE::SUCCESS) {
 				return cvmmap::unexpected(map_camera_control_operation_error(result, -EIO));
 			}
 			state.value = value;
 			return state;
 		}

 		int min_value = 0;
 		int max_value = 0;
 		const auto result = camera.getCameraSettings(descriptor.zed_setting, min_value, max_value);
 		if (result != sl::ERROR_CODE::SUCCESS) {
 			return cvmmap::unexpected(map_camera_control_operation_error(result, -EIO));
 		}
 		state.min_value = min_value;
 		state.max_value = max_value;
 		return state;
 	}

	uint64_t effective_timestamp_ns_locked() {
		return svo_mode ? zed_image_timestamp_ns(camera) : now_ns();
	}

	uint32_t next_frame_count() const {
		std::lock_guard lock(snapshot_mutex);
		return published.metadata.frame_count + 1;
	}

	std::chrono::milliseconds publish_gap_warning_threshold() const {
		const auto fps = std::max(1, options.zed_config.fps);
		const auto expected_publish_ms = std::max(1, (1000 / fps) * 5);
		return std::max(std::chrono::milliseconds(2000), std::chrono::milliseconds(expected_publish_ms));
	}

	bool uses_live_camera_recovery() const {
		return !svo_mode;
	}

	std::optional<std::string> fatal_capture_error_message(const sl::ERROR_CODE code) const {
		if (!uses_live_camera_recovery()) {
			return std::nullopt;
		}
		if (code != sl::ERROR_CODE::CAMERA_REBOOTING &&
			code != sl::ERROR_CODE::CORRUPTED_FRAME) {
			return std::nullopt;
		}
		return cvmmap::format(
			"ZED reported {} during capture; treating camera auto-recovery/corrupted output as fatal so the supervisor restarts the producer",
			sl::toString(code).get());
	}


	void stop_worker_thread() {
		if (worker_thread.joinable()) {
			worker_thread.request_stop();
			worker_thread.join();
		}
	}

	void start_worker_thread() {
		worker_thread = std::jthread([this](std::stop_token stop_token) {
			worker_loop(stop_token);
		});
	}

	source_info_t make_source_info_locked() const {
		source_info_t info{};
		info.source_kind = svo_mode ? cvmmap::SourceKind::Finite : cvmmap::SourceKind::Live;
		info.timestamp_domain = cvmmap::TimestampDomain::UnixEpochNs;
		info.flags |= cvmmap::SOURCE_INFO_FLAG_HAS_DEPTH;
		if (!svo_mode) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_RECORD;
		}
		if (svo_mode && options.video_config.finite_source_can_seek()) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_SEEK;
		}
		if (svo_mode && options.video_config.finite_source_auto_loops()) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_AUTO_LOOP;
		}
		if (svo_mode && options.video_config.finite_source_loop_emits_reset()) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_LOOP_EMITS_RESET;
		}
		if (options.zed_config.body_tracking &&
			options.zed_config.body_tracking->enabled) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_HAS_BODY;
		}
		if (svo_mode) {
			info.timeline_start_ns = timeline_start_ns;
			info.timeline_end_ns = timeline_end_ns;
			info.duration_ns =
				timeline_end_ns >= timeline_start_ns ? timeline_end_ns - timeline_start_ns : 0;
		}
		return info;
	}

	bool initialize_svo_timeline_locked() {
		if (!svo_mode || !camera.isOpened()) {
			timeline_start_ns = 0;
			timeline_end_ns = 0;
			total_svo_frames = 0;
			return true;
		}

		total_svo_frames = camera.getSVONumberOfFrames();
		if (total_svo_frames <= 0) {
			return false;
		}

		auto sample_timestamp_ns = [this](const int position) -> std::optional<uint64_t> {
			camera.setSVOPosition(position);
			const auto grab_result = camera.grab(runtime_parameters);
			if (grab_result != sl::ERROR_CODE::SUCCESS) {
				return std::nullopt;
			}
			const auto timestamp_ns = zed_image_timestamp_ns(camera);
			if (timestamp_ns == 0) {
				return std::nullopt;
			}
			return timestamp_ns;
		};

		const auto start_timestamp_ns = sample_timestamp_ns(0);
		if (!start_timestamp_ns) {
			return false;
		}
		timeline_start_ns = *start_timestamp_ns;

		const auto end_timestamp_ns =
			sample_timestamp_ns(std::max(0, total_svo_frames - 1));
		if (!end_timestamp_ns) {
			return false;
		}
		timeline_end_ns = *end_timestamp_ns;

		camera.setSVOPosition(0);
		return true;
	}

	recording_status_t make_recording_status_locked() {
		recording_status_t status{};
		status.format = cvmmap::RecordingFormat::Svo;
		status.can_record = !svo_mode;
		if (!camera.isOpened()) {
			return status;
		}

		const auto sdk_status = camera.getRecordingStatus();
		status.is_recording = sdk_status.is_recording;
		status.is_paused = sdk_status.is_paused;
		status.last_frame_ok = sdk_status.status;
		status.frames_ingested =
			sdk_status.number_frames_ingested < 0 ? 0u : static_cast<uint32_t>(sdk_status.number_frames_ingested);
		status.frames_encoded =
			sdk_status.number_frames_encoded < 0 ? 0u : static_cast<uint32_t>(sdk_status.number_frames_encoded);

		if (status.is_recording) {
			if (!active_recording_path.empty()) {
				status.active_path = active_recording_path;
			} else {
				status.active_path = camera.getRecordingParameters().video_filename.get();
			}
		}

		return status;
	}

	void stop_recording_locked(std::string_view reason) {
		if (!camera.isOpened()) {
			active_recording_path.clear();
			return;
		}

		const auto status = camera.getRecordingStatus();
		if (status.is_recording) {
			spdlog::warn("stopping ZED recording: {}", reason);
			camera.disableRecording();
		}
		active_recording_path.clear();
	}

	bool open_camera_locked() {
		init_parameters = sl::InitParameters{};
		init_parameters.camera_resolution = parse_resolution(options.zed_config.resolution);
		init_parameters.camera_fps = options.zed_config.fps;
		init_parameters.depth_mode = parse_depth_mode(options.zed_config.depth_mode);
		init_parameters.depth_stabilization = options.zed_config.depth_stabilization;
		depth_enabled = init_parameters.depth_mode != sl::DEPTH_MODE::NONE;
		body_tracking_enabled =
			options.zed_config.body_tracking.has_value() &&
			options.zed_config.body_tracking->enabled;
		svo_mode = is_svo_stream_mode(options.zed_config.stream_mode);
		if (!svo_mode) {
			init_parameters.async_grab_camera_recovery = true;
			init_parameters.enable_image_validity_check = 1;
		}

		if (body_tracking_enabled) {
			init_parameters.coordinate_units = sl::UNIT::METER;
			init_parameters.coordinate_system =
				parse_coordinate_system(options.zed_config.coordinate_system);
			body_reference_frame = parse_body_reference_frame(
				options.zed_config.body_tracking->reference_frame);
			runtime_parameters.measure3D_reference_frame = body_reference_frame;
		}

		const auto stream_mode = normalize_ascii_lower(options.zed_config.stream_mode);

		if (svo_mode) {
			if (!options.zed_config.svo_path || options.zed_config.svo_path->empty()) {
				spdlog::error("ZED stream_mode='svo' requires svo_path");
				return false;
			}
			init_parameters.svo_real_time_mode = options.zed_config.svo_real_time_mode;
			spdlog::info("opening ZED SVO playback '{}'", *options.zed_config.svo_path);
			init_parameters.input.setFromSVOFile(sl::String(options.zed_config.svo_path->c_str()));
		} else if (is_network_stream_mode(stream_mode)) {
			eth_stubs.log_placeholder_status();

			if (!options.zed_config.ip_address || options.zed_config.ip_address->empty()) {
				spdlog::error("ZED stream_mode='{}' requires ip_address", options.zed_config.stream_mode);
				return false;
			}

			if (options.zed_config.port) {
				spdlog::info("[NETWORK MODE: {}] opening ZED network stream {}:{}",
					options.zed_config.stream_mode,
					*options.zed_config.ip_address,
					*options.zed_config.port);
				init_parameters.input.setFromStream(sl::String(options.zed_config.ip_address->c_str()), static_cast<unsigned short>(*options.zed_config.port));
			} else {
				spdlog::info("[NETWORK MODE: {}] opening ZED network stream {} (default port)",
					options.zed_config.stream_mode,
					*options.zed_config.ip_address);
				init_parameters.input.setFromStream(sl::String(options.zed_config.ip_address->c_str()));
			}
		} else {
			if (options.zed_config.serial) {
				spdlog::info("[LOCAL MODE: {}] opening ZED local camera by serial={}",
					options.zed_config.stream_mode,
					*options.zed_config.serial);
				init_parameters.input.setFromSerialNumber(*options.zed_config.serial);
			} else if (options.zed_config.index) {
				spdlog::info("[LOCAL MODE: {}] opening ZED local camera by index={}",
					options.zed_config.stream_mode,
					*options.zed_config.index);
				init_parameters.input.setFromCameraID(*options.zed_config.index);
			} else {
				spdlog::info("[LOCAL MODE: {}] opening ZED local camera by default selection",
					options.zed_config.stream_mode);
			}
		}

		left_view = parse_left_view(options.zed_config.left_pixel_format);

		auto open_result = sl::ERROR_CODE::FAILURE;
		const auto timeout_ms = std::max(1, options.zed_config.open_timeout_ms);
		auto started = std::chrono::steady_clock::now();

		while (true) {
			open_result = camera.open(init_parameters);
			if (open_result == sl::ERROR_CODE::SUCCESS) {
				if (body_tracking_enabled) {
					positional_tracking_parameters = sl::PositionalTrackingParameters{};
					positional_tracking_parameters.set_floor_as_origin =
						options.zed_config.body_tracking->set_floor_as_origin;
					auto positional_result =
						camera.enablePositionalTracking(positional_tracking_parameters);
					if (positional_result != sl::ERROR_CODE::SUCCESS) {
						spdlog::error("failed to enable ZED positional tracking: code={}", static_cast<int>(positional_result));
						camera.close();
						return false;
					}

					const auto &cfg = *options.zed_config.body_tracking;
					body_tracking_parameters = sl::BodyTrackingParameters{};
					body_tracking_parameters.enable_tracking = true;
					body_tracking_parameters.enable_segmentation = false;
					body_tracking_parameters.detection_model = parse_body_tracking_model(cfg.detection_model);
					body_tracking_parameters.enable_body_fitting = cfg.enable_body_fitting;
					body_tracking_parameters.body_format = parse_body_format(cfg.body_format);
					body_tracking_parameters.body_selection = parse_body_selection(cfg.body_selection);
					body_tracking_parameters.max_range = cfg.max_range;
					body_tracking_parameters.prediction_timeout_s = cfg.prediction_timeout_s;
					body_tracking_parameters.allow_reduced_precision_inference =
						cfg.allow_reduced_precision_inference;

					body_tracking_runtime_parameters = sl::BodyTrackingRuntimeParameters{};
					body_tracking_runtime_parameters.detection_confidence_threshold =
						cfg.detection_confidence_threshold;
					body_tracking_runtime_parameters.minimum_keypoints_threshold =
						cfg.minimum_keypoints_threshold;
					body_tracking_runtime_parameters.skeleton_smoothing =
						cfg.skeleton_smoothing;

					auto body_tracking_result =
						camera.enableBodyTracking(body_tracking_parameters);
					if (body_tracking_result != sl::ERROR_CODE::SUCCESS) {
						spdlog::error("failed to enable ZED body tracking: code={}", static_cast<int>(body_tracking_result));
						camera.disablePositionalTracking();
						camera.close();
						return false;
					}
				}
				spdlog::info("ZED camera opened");
				if (!initialize_svo_timeline_locked()) {
					if (camera.isOpened()) {
						camera.close();
					}
					spdlog::error("failed to initialize ZED SVO timeline");
					return false;
				}
				return true;
			}

			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started)
						 .count();
			if (elapsed >= timeout_ms) {
				break;
			}

			if (camera.isOpened()) {
				camera.close();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}

		spdlog::error("failed to open ZED camera within {}ms: code={}", timeout_ms, static_cast<int>(open_result));
		return false;
	}

	void warmup_camera_locked() {
		if (svo_mode) {
			return;
		}
		const auto warmup_frames = std::max(0, options.zed_config.warmup_frames);
		for (int i = 0; i < warmup_frames; i++) {
			camera.grab(runtime_parameters);
		}
	}

	std::optional<size_t> expected_row_bytes(const sl::Mat &frame) const {
		const auto width = frame.getWidth();
		if (width <= 0) {
			return std::nullopt;
		}

		const auto channels = channels_from_mat_type(frame.getDataType());
		auto depth = depth_from_mat_type(frame.getDataType());
		if (channels == 0 || !depth) {
			return std::nullopt;
		}

		return static_cast<size_t>(width) * static_cast<size_t>(channels) * static_cast<size_t>(size_of(*depth));
	}

	bool copy_compact_plane(const sl::Mat &src, const size_t row_bytes, std::span<uint8_t> dst) const {
		const auto height = src.getHeight();
		auto *src_ptr = src.getPtr<sl::uchar1>(sl::MEM::CPU);
		const auto step = src.getStepBytes(sl::MEM::CPU);
		if (height <= 0 || !src_ptr || row_bytes == 0 || step < row_bytes) {
			return false;
		}

		const auto expected_size = row_bytes * static_cast<size_t>(height);
		if (dst.size() != expected_size) {
			return false;
		}

		if (step == row_bytes) {
			std::memcpy(dst.data(), src_ptr, expected_size);
			return true;
		}

		for (int row = 0; row < height; row++) {
			const auto src_offset = static_cast<size_t>(row) * step;
			const auto dst_offset = static_cast<size_t>(row) * row_bytes;
			std::memcpy(dst.data() + dst_offset, src_ptr + src_offset, row_bytes);
		}

		return true;
	}

	std::optional<cvmmap::body_tracking_frame_t> capture_body_tracking_frame_locked(
		const uint32_t frame_count,
		const uint64_t timestamp_ns) {
		if (!body_tracking_enabled) {
			return std::nullopt;
		}

		auto retrieve_result = camera.retrieveBodies(
			bodies,
			body_tracking_runtime_parameters,
			body_tracking_parameters.instance_module_id);
		if (retrieve_result != sl::ERROR_CODE::SUCCESS) {
			spdlog::warn("ZED retrieveBodies failed: code={}", static_cast<int>(retrieve_result));
			return std::nullopt;
		}

		cvmmap::body_tracking_frame_t frame{};
		frame.header._magic = cvmmap::BODY_TRACKING_MAGIC;
		frame.header.versions_major = VERSION_MAJOR;
		frame.header.versions_minor = VERSION_MINOR;
		frame.header.frame_count = frame_count;
		frame.header.timestamp_ns = timestamp_ns;
		frame.header.sdk_timestamp_ns =
			static_cast<uint64_t>(bodies.timestamp.getNanoseconds());
		frame.header.body_record_size = sizeof(cvmmap::body_tracking_body_t);
		frame.header.body_format = to_body_format(bodies.body_format);
		frame.header.body_selection = to_body_selection(body_tracking_parameters.body_selection);
		frame.header.detection_model = to_body_tracking_model(body_tracking_parameters.detection_model);
		frame.header.inference_precision = to_inference_precision(bodies.inference_precision_mode);
		frame.header.flags = 0;
		frame.header.set_coordinate_system(
			to_body_coordinate_system(init_parameters.coordinate_system));
		frame.header.set_reference_frame(
			to_body_reference_frame(body_reference_frame));
		frame.header.set_floor_as_origin(
			options.zed_config.body_tracking &&
			options.zed_config.body_tracking->set_floor_as_origin);
		if (bodies.is_new) {
			frame.header.flags |= cvmmap::BODY_TRACKING_FLAG_IS_NEW;
		}
		if (bodies.is_tracked) {
			frame.header.flags |= cvmmap::BODY_TRACKING_FLAG_IS_TRACKED;
		}
		if (body_tracking_parameters.enable_body_fitting) {
			frame.header.flags |= cvmmap::BODY_TRACKING_FLAG_BODY_FITTING_ENABLED;
		}
		if (body_tracking_parameters.allow_reduced_precision_inference) {
			frame.header.flags |= cvmmap::BODY_TRACKING_FLAG_REDUCED_PRECISION_REQUESTED;
		}
		std::memset(frame.header._label, 0, sizeof(frame.header._label));
		std::memcpy(
			frame.header._label,
			options.zed_config.body_tracking ? options.zed_config.body_tracking->body_format.c_str() : "",
			std::min(sizeof(frame.header._label),
					 options.zed_config.body_tracking ? options.zed_config.body_tracking->body_format.size() : static_cast<size_t>(0)));

		if (!bodies.is_new) {
			frame.header.body_count = 0;
			frame.header.payload_size_bytes = 0;
			return frame;
		}

		frame.bodies.resize(std::min(
			bodies.body_list.size(),
			static_cast<size_t>(std::numeric_limits<uint16_t>::max())));
		for (size_t i = 0; i < frame.bodies.size(); ++i) {
			copy_body_record(frame.bodies[i], bodies.body_list[i]);
		}
		frame.header.body_count = static_cast<uint16_t>(frame.bodies.size());
		frame.header.payload_size_bytes =
			static_cast<uint32_t>(frame.bodies.size() * sizeof(cvmmap::body_tracking_body_t));
		return frame;
	}

	std::optional<size_t> pack_frame_locked(std::span<uint8_t> payload, frame_info_t &info_out) {
		const auto height = left_frame.getHeight();
		const auto left_row_bytes = expected_row_bytes(left_frame);
		if (!left_row_bytes || height <= 0) {
			spdlog::error("invalid left frame geometry/type for compact packing");
			return std::nullopt;
		}

		const auto packed_left_size = (*left_row_bytes) * static_cast<size_t>(height);
		if (packed_left_size == 0) {
			spdlog::error("left frame compact size is zero");
			return std::nullopt;
		}

		size_t packed_depth_size = 0;
		size_t packed_confidence_size = 0;
		size_t depth_row_bytes = 0;
		bool depth_plane_available = false;
		bool confidence_plane_available = false;

		if (depth_enabled) {
			depth_row_bytes = static_cast<size_t>(left_frame.getWidth()) * sizeof(float);
			const size_t depth_total_bytes = depth_row_bytes * static_cast<size_t>(height);
			if (depth_total_bytes == 0) {
				spdlog::error("depth plane compact size is zero while depth is enabled");
				return std::nullopt;
			}

			packed_depth_size = depth_total_bytes;
			const auto depth_result = camera.retrieveMeasure(
				depth_frame,
				sl::MEASURE::DEPTH,
				sl::MEM::CPU,
				sl::Resolution(left_frame.getWidth(), left_frame.getHeight()));

			const auto depth_geometry_valid = depth_frame.getDataType() == sl::MAT_TYPE::F32_C1 &&
				depth_frame.getWidth() == left_frame.getWidth() &&
				depth_frame.getHeight() == left_frame.getHeight();
			depth_plane_available = depth_result == sl::ERROR_CODE::SUCCESS && depth_geometry_valid;
			if (!depth_plane_available) {
				if (depth_result != sl::ERROR_CODE::SUCCESS) {
					spdlog::warn("ZED retrieveMeasure(DEPTH) failed: code={}; using stable fallback depth bytes", static_cast<int>(depth_result));
				} else if (!depth_geometry_valid) {
					spdlog::warn(
						"ZED depth plane shape/type mismatch (type={}, {}x{} vs left {}x{}); using stable fallback depth bytes",
						static_cast<int>(depth_frame.getDataType()),
						depth_frame.getWidth(),
						depth_frame.getHeight(),
						left_frame.getWidth(),
						left_frame.getHeight());
				}
			}

			if (options.zed_config.publish_confidence) {
				const auto confidence_result = camera.retrieveMeasure(
					confidence_frame,
					sl::MEASURE::CONFIDENCE,
					sl::MEM::CPU,
					sl::Resolution(left_frame.getWidth(), left_frame.getHeight()));

				const auto confidence_geometry_valid =
					confidence_frame.getDataType() == sl::MAT_TYPE::F32_C1 &&
					confidence_frame.getWidth() == left_frame.getWidth() &&
					confidence_frame.getHeight() == left_frame.getHeight();
				confidence_plane_available = confidence_result == sl::ERROR_CODE::SUCCESS && confidence_geometry_valid;
				if (confidence_plane_available) {
					packed_confidence_size = depth_total_bytes;
				} else if (confidence_result != sl::ERROR_CODE::SUCCESS) {
					spdlog::debug(
						"ZED retrieveMeasure(CONFIDENCE) unavailable: code={}; publishing left/depth only",
						static_cast<int>(confidence_result));
				} else if (!confidence_geometry_valid) {
					spdlog::warn(
						"ZED confidence plane shape/type mismatch (type={}, {}x{} vs left {}x{}); publishing left/depth only",
						static_cast<int>(confidence_frame.getDataType()),
						confidence_frame.getWidth(),
						confidence_frame.getHeight(),
						left_frame.getWidth(),
						left_frame.getHeight());
				}
			}
		}

		const auto packed_size = packed_left_size + packed_depth_size + packed_confidence_size;
		if (packed_size == 0 || packed_size > std::numeric_limits<uint32_t>::max()) {
			spdlog::error(
				"invalid packed frame size: left={} depth={} confidence={} total={}",
				packed_left_size,
				packed_depth_size,
				packed_confidence_size,
				packed_size);
			return std::nullopt;
		}
		if (payload.size() < packed_size) {
			spdlog::error("packed output buffer too small: capacity={} required={}", payload.size(), packed_size);
			return std::nullopt;
		}

		if (!copy_compact_plane(left_frame, *left_row_bytes, payload.subspan(0, packed_left_size))) {
			spdlog::error("failed to compact/copy left plane into packed payload");
			return std::nullopt;
		}

		if (depth_enabled) {
			auto depth_payload = payload.subspan(packed_left_size, packed_depth_size);
			if (depth_plane_available && copy_compact_plane(depth_frame, depth_row_bytes, depth_payload)) {
				last_good_depth_plane.resize(packed_depth_size);
				std::memcpy(last_good_depth_plane.data(), depth_payload.data(), packed_depth_size);
			} else {
				if (depth_plane_available) {
					spdlog::warn("ZED depth plane compaction failed; using stable fallback depth bytes");
				}
				if (last_good_depth_plane.size() == packed_depth_size) {
					std::memcpy(depth_payload.data(), last_good_depth_plane.data(), packed_depth_size);
				} else {
					std::fill(depth_payload.begin(), depth_payload.end(), 0);
				}
			}
		}
		if (packed_confidence_size > 0) {
			auto confidence_payload = payload.subspan(packed_left_size + packed_depth_size, packed_confidence_size);
			if (!copy_compact_plane(confidence_frame, depth_row_bytes, confidence_payload)) {
				spdlog::warn("ZED confidence plane compaction failed; publishing left/depth only");
				packed_confidence_size = 0;
			}
		}

		info_out.buffer_size = static_cast<uint32_t>(packed_left_size + packed_depth_size + packed_confidence_size);
		return static_cast<size_t>(info_out.buffer_size);
	}

	cvmmap::expected<CapturedFrame, sl::ERROR_CODE> capture_frame_locked(
		const uint32_t frame_count) {
		const auto grab_result = camera.grab(runtime_parameters);
		if (grab_result != sl::ERROR_CODE::SUCCESS) {
			spdlog::debug("ZED grab failed: code={}", static_cast<int>(grab_result));
			return cvmmap::unexpected(grab_result);
		}

		const auto retrieve_result = camera.retrieveImage(left_frame, left_view, sl::MEM::CPU);
		if (retrieve_result != sl::ERROR_CODE::SUCCESS) {
			spdlog::debug("ZED retrieveImage failed: code={}", static_cast<int>(retrieve_result));
			return cvmmap::unexpected(retrieve_result);
		}

		auto info = make_frame_info(left_frame);
		if (!info) {
			spdlog::error("invalid left frame geometry/type for compact packing");
			return cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
		}

		CapturedFrame captured{};
		captured.info = *info;
		const size_t depth_plane_bytes = static_cast<size_t>(captured.info.width) * static_cast<size_t>(captured.info.height) * sizeof(float);
		size_t raw_capacity = captured.info.buffer_size;
		if (depth_enabled) {
			raw_capacity += depth_plane_bytes;
			if (options.zed_config.publish_confidence) {
				raw_capacity += depth_plane_bytes;
			}
		}
		if (raw_capacity > std::numeric_limits<uint32_t>::max()) {
			spdlog::error("invalid ZED packed frame capacity: {}", raw_capacity);
			return cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
		}
		captured.info.buffer_size = static_cast<uint32_t>(raw_capacity);
		captured.timestamp_ns = effective_timestamp_ns_locked();
		captured.source_info = make_source_info_locked();
		if (body_tracking_enabled) {
			captured.body_tracking = capture_body_tracking_frame_locked(frame_count, captured.timestamp_ns);
		}
		return captured;
	}

	std::optional<frame_info_t> make_frame_info(const sl::Mat &frame) {
		const auto width = frame.getWidth();
		const auto height = frame.getHeight();
		if (width <= 0 || height <= 0 ||
			width > std::numeric_limits<uint16_t>::max() ||
			height > std::numeric_limits<uint16_t>::max()) {
			spdlog::error("invalid ZED frame dimensions: {}x{}", width, height);
			return std::nullopt;
		}

		const auto mat_type = frame.getDataType();
		const auto channels = channels_from_mat_type(mat_type);
		auto depth = depth_from_mat_type(mat_type);
		if (channels == 0) {
			spdlog::error("unsupported ZED MAT_TYPE: {}", static_cast<int>(mat_type));
			return std::nullopt;
		}
		if (!depth) {
			spdlog::error("unsupported ZED MAT_TYPE depth mapping: {}", static_cast<int>(mat_type));
			return std::nullopt;
		}

		auto row_bytes = expected_row_bytes(frame);
		if (!row_bytes) {
			spdlog::error("cannot compute compact row bytes for ZED MAT_TYPE: {}", static_cast<int>(mat_type));
			return std::nullopt;
		}

		const auto buffer_size = (*row_bytes) * static_cast<size_t>(height);
		if (buffer_size == 0 || buffer_size > std::numeric_limits<uint32_t>::max()) {
			spdlog::error("invalid ZED frame buffer size: {}", buffer_size);
			return std::nullopt;
		}

		return frame_info_t{
			.width = static_cast<uint16_t>(width),
			.height = static_cast<uint16_t>(height),
			.channels = channels,
			.depth = *depth,
			.pixel_format = guess_pixel_format_for_zed(mat_type),
			.buffer_size = static_cast<uint32_t>(buffer_size),
		};
	}

	PublishedFrame publish_captured_frame(
		CapturedFrame captured,
		std::vector<uint8_t> payload,
		const uint32_t frame_count,
		const bool log_publish_gap) {
		PublishedFrame published_frame{};
		published_frame.metadata.frame_count = frame_count;
		published_frame.metadata.timestamp_ns = captured.timestamp_ns;
		published_frame.metadata.info = captured.info;
		published_frame.metadata.info.buffer_size = static_cast<uint32_t>(payload.size());
		published_frame.payload = std::move(payload);
		published_frame.body_tracking = std::move(captured.body_tracking);

		std::optional<int64_t> publish_gap_ms;
		{
			std::lock_guard lock(snapshot_mutex);
			published.metadata = published_frame.metadata;
			published.source_info = captured.source_info;
			published.source_info.current_timestamp_ns = published_frame.metadata.timestamp_ns;
			published.source_info.current_frame_count = published_frame.metadata.frame_count;
			if (log_publish_gap && published.has_last_publish_at) {
				const auto gap = std::chrono::steady_clock::now() - published.last_publish_at;
				if (gap > publish_gap_warning_threshold()) {
					publish_gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(gap).count();
				}
			}
			published.last_publish_at = std::chrono::steady_clock::now();
			published.has_last_publish_at = true;
		}

		if (publish_gap_ms) {
			spdlog::warn(
				"ZED publish gap detected: {}ms before frame {} timestamp {}",
				*publish_gap_ms,
				published_frame.metadata.frame_count,
				published_frame.metadata.timestamp_ns);
		}

		return published_frame;
	}

	DirectPublishedFrame publish_captured_frame_direct(
		CapturedFrame captured,
		const uint32_t frame_count,
		const bool log_publish_gap) {
		DirectPublishedFrame published_frame{};
		published_frame.metadata.frame_count = frame_count;
		published_frame.metadata.timestamp_ns = captured.timestamp_ns;
		published_frame.metadata.info = captured.info;
		published_frame.body_tracking = std::move(captured.body_tracking);
		published_frame.fill_payload = [this, info = published_frame.metadata.info](std::span<uint8_t> output_buffer) mutable -> std::optional<size_t> {
			std::lock_guard lock(camera_mutex);
			return pack_frame_locked(output_buffer, info);
		};

		std::optional<int64_t> publish_gap_ms;
		{
			std::lock_guard lock(snapshot_mutex);
			published.metadata = published_frame.metadata;
			published.source_info = captured.source_info;
			published.source_info.current_timestamp_ns = published_frame.metadata.timestamp_ns;
			published.source_info.current_frame_count = published_frame.metadata.frame_count;
			if (log_publish_gap && published.has_last_publish_at) {
				const auto gap = std::chrono::steady_clock::now() - published.last_publish_at;
				if (gap > publish_gap_warning_threshold()) {
					publish_gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(gap).count();
				}
			}
			published.last_publish_at = std::chrono::steady_clock::now();
			published.has_last_publish_at = true;
		}

		if (publish_gap_ms) {
			spdlog::warn(
				"ZED publish gap detected: {}ms before frame {} timestamp {}",
				*publish_gap_ms,
				published_frame.metadata.frame_count,
				published_frame.metadata.timestamp_ns);
		}

		return published_frame;
	}

	bool reconnect() {
		if (is_svo_stream_mode(options.zed_config.stream_mode)) {
			return false;
		}

		std::lock_guard lock(camera_mutex);
		if (camera.isOpened()) {
			stop_recording_locked("capture reconnect");
			if (body_tracking_enabled) {
				camera.disableBodyTracking(body_tracking_parameters.instance_module_id);
				camera.disablePositionalTracking();
			}
			camera.close();
		}

		const auto sleep_ms = std::max(1, options.zed_config.reconnect_interval_ms);
		std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

		if (!open_camera_locked()) {
			return false;
		}

		warmup_camera_locked();
		return true;
	}

	void emit_published_frame(PublishedFrame &published_frame) {
		on_frame(
			std::span<uint8_t>(
				published_frame.payload.data(),
				published_frame.payload.size()),
			published_frame.metadata);
		if (published_frame.body_tracking) {
			on_body_tracking(*published_frame.body_tracking);
		}
	}

	void emit_published_frame_direct(DirectPublishedFrame published_frame) {
		on_frame_direct(ZedDirectFrame{
			.metadata = published_frame.metadata,
			.fill_payload = std::move(published_frame.fill_payload),
		});
		if (published_frame.body_tracking) {
			on_body_tracking(*published_frame.body_tracking);
		}
	}

	void Init() {
		if (initialized.load(std::memory_order_relaxed)) {
			return;
		}

		cvmmap::expected<CapturedFrame, sl::ERROR_CODE> initial_capture = cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
		{
			std::lock_guard lock(camera_mutex);
			if (!open_camera_locked()) {
				on_error(-ENODEV, "Failed to open ZED camera");
				return;
			}

			warmup_camera_locked();
			initial_capture = capture_frame_locked(0);
			if (!initial_capture) {
				spdlog::error("failed to capture first frame from ZED");
				if (const auto fatal_capture_message =
						fatal_capture_error_message(initial_capture.error());
					fatal_capture_message) {
					on_error(ERR_FATAL_CAMERA_RECOVERY, *fatal_capture_message);
				} else {
					const auto message = cvmmap::format(
						"Failed to capture first frame: {}",
						sl::toString(initial_capture.error()).get());
					on_error(-EIO, message);
				}
				camera.close();
				return;
			}
		}

		auto initial_direct = publish_captured_frame_direct(std::move(*initial_capture), 0, false);

		spdlog::info("initial ZED frame info: {}x{}x{}; depth={}; bufferSize={}; pixelFormat={}; depthPlaneEnabled={}",
					 initial_direct.metadata.info.width,
					 initial_direct.metadata.info.height,
					 initial_direct.metadata.info.channels,
					 app::to_str(initial_direct.metadata.info.depth),
					 initial_direct.metadata.info.buffer_size,
					 app::to_str(initial_direct.metadata.info.pixel_format),
					 depth_enabled);

		on_metadata(initial_direct.metadata);
		if (_on_frame_direct) {
			emit_published_frame_direct(std::move(initial_direct));
		} else {
			std::vector<uint8_t> payload(initial_direct.metadata.info.buffer_size);
			auto packed_size = initial_direct.fill_payload(std::span<uint8_t>(payload.data(), payload.size()));
			if (!packed_size) {
				spdlog::error("failed to pack initial ZED frame payload");
				return;
			}
			payload.resize(*packed_size);
			auto initial_published = publish_captured_frame(
				CapturedFrame{
					.info = initial_direct.metadata.info,
					.source_info = initial_capture->source_info,
					.payload = {},
					.body_tracking = std::move(initial_direct.body_tracking),
					.timestamp_ns = initial_direct.metadata.timestamp_ns,
				},
				std::move(payload),
				0,
				false);
			emit_published_frame(initial_published);
		}

		initialized.store(true, std::memory_order_relaxed);
		start_worker_thread();
	}

	void worker_loop(std::stop_token stop_token) {
		const auto max_failures = std::max(1, options.zed_config.max_consecutive_failures);
		const auto finite_source = is_svo_stream_mode(options.zed_config.stream_mode);
		int consecutive_failures = 0;

		while (!stop_token.stop_requested()) {
			const auto frame_count = next_frame_count();
			cvmmap::expected<CapturedFrame, sl::ERROR_CODE> capture = cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
			{
				std::lock_guard lock(camera_mutex);
				capture = capture_frame_locked(frame_count);
			}

			if (capture) {
				consecutive_failures = 0;
				if (_on_frame_direct) {
					auto published_frame = publish_captured_frame_direct(std::move(*capture), frame_count, true);
					emit_published_frame_direct(std::move(published_frame));
					spdlog::debug("frame@{}", frame_count);
				} else {
					std::vector<uint8_t> payload(capture->info.buffer_size);
					auto packed_size = pack_frame_locked(std::span<uint8_t>(payload.data(), payload.size()), capture->info);
					if (!packed_size) {
						spdlog::error("failed to pack ZED frame payload");
						continue;
					}
					payload.resize(*packed_size);
					auto published_frame = publish_captured_frame(std::move(*capture), std::move(payload), frame_count, true);
					emit_published_frame(published_frame);
					spdlog::debug("frame@{}", published_frame.metadata.frame_count);
				}
				continue;
			}

			if (const auto fatal_capture_message =
					fatal_capture_error_message(capture.error());
				fatal_capture_message) {
				on_error(ERR_FATAL_CAMERA_RECOVERY, *fatal_capture_message);
				break;
			}

			consecutive_failures++;
			if (finite_source && capture.error() == sl::ERROR_CODE::END_OF_SVOFILE_REACHED) {
				on_error(ERR_EOS, "EOF");
				break;
			}
			if (consecutive_failures < max_failures) {
				continue;
			}

			spdlog::error("ZED capture failed {} consecutive times", consecutive_failures);
			if (!options.zed_config.reconnect) {
				on_error(-EIO, "ZED capture failure");
				break;
			}

			spdlog::warn("attempting ZED reconnect");
			if (!reconnect()) {
				on_error(-EIO, "ZED reconnect failed");
				continue;
			}
			consecutive_failures = 0;
		}
	}

	void SetOnFrameDirect(on_zed_frame_direct_fn_t on_frame_direct_) {
		_on_frame_direct = std::move(on_frame_direct_);
	}

	void Shutdown() {
		stop_worker_thread();

		std::lock_guard lock(camera_mutex);
		if (camera.isOpened()) {
			stop_recording_locked("backend shutdown");
			camera.close();
		}

		initialized.store(false, std::memory_order_relaxed);
	}

	void SetOnMetadata(on_metadata_fn_t on_metadata_) {
		_on_metadata = std::move(on_metadata_);
	}

	void SetOnFrame(on_frame_fn_t on_frame_) {
		_on_frame = std::move(on_frame_);
	}

	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking_) {
		_on_body_tracking = std::move(on_body_tracking_);
	}

	void SetOnError(on_error_fn_t on_error_) {
		_on_error = std::move(on_error_);
	}

	source_info_t GetSourceInfo() {
		std::lock_guard lock(snapshot_mutex);
		return published.source_info;
	}

	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns) {
		if (!is_svo_stream_mode(options.zed_config.stream_mode) || !options.video_config.finite_source_can_seek()) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}

		stop_worker_thread();

		cvmmap::expected<CapturedFrame, sl::ERROR_CODE> capture = cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
		bool restart_worker = false;
		uint64_t landed_timestamp_ns = 0;
		{
			std::lock_guard lock(camera_mutex);
			restart_worker =
				initialized.load(std::memory_order_relaxed) &&
				camera.isOpened() &&
				total_svo_frames > 0;
			if (!restart_worker) {
				return cvmmap::unexpected(-ENODEV);
			}

			const auto clamped_timestamp_ns =
				std::clamp(timestamp_ns, timeline_start_ns, timeline_end_ns);
			const sl::Timestamp target_timestamp =
				static_cast<uint64_t>(clamped_timestamp_ns);

			int target_position = camera.getSVOPositionAtTimestamp(target_timestamp);
			if (target_position < 0) {
				target_position = clamped_timestamp_ns <= timeline_start_ns ?
					0 :
					std::max(0, total_svo_frames - 1);
			}

			camera.setSVOPosition(target_position);
			last_good_depth_plane.clear();
			capture = capture_frame_locked(0);
			if (capture) {
				landed_timestamp_ns = capture->timestamp_ns;
			}
		}

		if (!capture) {
			if (restart_worker) {
				start_worker_thread();
			}
			return cvmmap::unexpected(
				capture.error() == sl::ERROR_CODE::END_OF_SVOFILE_REACHED ? -ERANGE : -EIO);
		}

		auto published_frame = publish_captured_frame_direct(std::move(*capture), 0, false);
		if (_on_frame_direct) {
			emit_published_frame_direct(std::move(published_frame));
		} else {
			std::vector<uint8_t> payload(published_frame.metadata.info.buffer_size);
			auto packed_size = published_frame.fill_payload(std::span<uint8_t>(payload.data(), payload.size()));
			if (!packed_size) {
				if (restart_worker) {
					start_worker_thread();
				}
				return cvmmap::unexpected(-EIO);
			}
			payload.resize(*packed_size);
			auto fallback = publish_captured_frame(
				CapturedFrame{
					.info = published_frame.metadata.info,
					.source_info = capture->source_info,
					.payload = {},
					.body_tracking = std::move(published_frame.body_tracking),
					.timestamp_ns = published_frame.metadata.timestamp_ns,
				},
				std::move(payload),
				0,
				false);
			emit_published_frame(fallback);
		}
		if (restart_worker) {
			start_worker_thread();
		}
		return seek_result_t{
			.requested_timestamp_ns = timestamp_ns,
			.landed_timestamp_ns = landed_timestamp_ns,
			.landed_frame_count = published_frame.metadata.frame_count,
			.exact_match = (landed_timestamp_ns == timestamp_ns),
		};
	}

	error_t ResetFrameCount() {
		if (!is_svo_stream_mode(options.zed_config.stream_mode)) {
			std::lock_guard lock(snapshot_mutex);
			published.metadata.frame_count = 0;
			published.source_info.current_frame_count = 0;
			return 0;
		}

		stop_worker_thread();

		cvmmap::expected<CapturedFrame, sl::ERROR_CODE> capture = cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
		bool restart_worker = false;
		{
			std::lock_guard lock(camera_mutex);
			restart_worker =
				initialized.load(std::memory_order_relaxed) &&
				camera.isOpened() &&
				total_svo_frames > 0;
			if (!restart_worker) {
				return -ENODEV;
			}

			camera.setSVOPosition(0);
			last_good_depth_plane.clear();
			capture = capture_frame_locked(0);
		}

		if (!capture) {
			return capture.error() == sl::ERROR_CODE::END_OF_SVOFILE_REACHED ? -ERANGE : -EIO;
		}

		auto published_frame = publish_captured_frame_direct(std::move(*capture), 0, false);
		if (_on_frame_direct) {
			emit_published_frame_direct(std::move(published_frame));
		} else {
			std::vector<uint8_t> payload(published_frame.metadata.info.buffer_size);
			auto packed_size = published_frame.fill_payload(std::span<uint8_t>(payload.data(), payload.size()));
			if (!packed_size) {
				return -EIO;
			}
			payload.resize(*packed_size);
			auto fallback = publish_captured_frame(
				CapturedFrame{
					.info = published_frame.metadata.info,
					.source_info = make_source_info_locked(),
					.payload = {},
					.body_tracking = std::move(published_frame.body_tracking),
					.timestamp_ns = published_frame.metadata.timestamp_ns,
				},
				std::move(payload),
				0,
				false);
			emit_published_frame(fallback);
		}
		if (restart_worker) {
			start_worker_thread();
		}
		return ERR_OK;
	}

	camera_control_capabilities_t GetCameraControlCapabilities() {
		std::lock_guard lock(camera_mutex);
		camera_control_capabilities_t capabilities{};
		if (camera_control_availability_error_locked() != ERR_OK) {
			return capabilities;
		}

		const auto camera_model = camera.getCameraInformation().camera_model;
		for (const auto &descriptor : ZED_CAMERA_CONTROL_DESCRIPTORS) {
			if (!is_zed_camera_control_supported(descriptor, camera_model)) {
				continue;
			}
			if (!can_read_camera_control_locked(descriptor)) {
				continue;
			}
			capabilities.supported_settings.push_back(descriptor.setting);
		}
		capabilities.supported = !capabilities.supported_settings.empty();
		return capabilities;
	}

	cvmmap::expected<camera_control_state_t, error_t> GetCameraControl(cvmmap::CameraControlSetting setting) {
		std::lock_guard lock(camera_mutex);
		if (const auto availability_error = camera_control_availability_error_locked(); availability_error != ERR_OK) {
			return cvmmap::unexpected(availability_error);
		}

		const auto *descriptor = find_zed_camera_control_descriptor(setting);
		if (descriptor == nullptr) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}

		const auto camera_model = camera.getCameraInformation().camera_model;
		if (!is_zed_camera_control_supported(*descriptor, camera_model) || !can_read_camera_control_locked(*descriptor)) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}

		return get_camera_control_locked(*descriptor);
	}

	cvmmap::expected<camera_control_state_t, error_t> SetCameraControl(const camera_control_request_t &request) {
		std::lock_guard lock(camera_mutex);
		if (const auto availability_error = camera_control_availability_error_locked(); availability_error != ERR_OK) {
			return cvmmap::unexpected(availability_error);
		}

		const auto *descriptor = find_zed_camera_control_descriptor(request.setting);
		if (descriptor == nullptr || descriptor->kind != cvmmap::CameraControlValueKind::Single) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}

		const auto camera_model = camera.getCameraInformation().camera_model;
		if (!is_zed_camera_control_supported(*descriptor, camera_model) || !can_read_camera_control_locked(*descriptor)) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}

		if (request.mode != cvmmap::CameraControlWriteMode::Manual &&
			request.mode != cvmmap::CameraControlWriteMode::Auto) {
			return cvmmap::unexpected(-EINVAL);
		}

		const int value = request.mode == cvmmap::CameraControlWriteMode::Auto ?
			sl::VIDEO_SETTINGS_VALUE_AUTO :
			request.value;
		const auto result = camera.setCameraSettings(descriptor->zed_setting, value);
		if (result != sl::ERROR_CODE::SUCCESS) {
			return cvmmap::unexpected(map_camera_control_operation_error(result, -ERANGE));
		}

		return get_camera_control_locked(*descriptor);
	}

	cvmmap::expected<camera_control_state_t, error_t> SetCameraControlRange(
		const camera_control_range_request_t &request) {
		std::lock_guard lock(camera_mutex);
		if (const auto availability_error = camera_control_availability_error_locked(); availability_error != ERR_OK) {
			return cvmmap::unexpected(availability_error);
		}

		const auto *descriptor = find_zed_camera_control_descriptor(request.setting);
		if (descriptor == nullptr || descriptor->kind != cvmmap::CameraControlValueKind::Range) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}

		const auto camera_model = camera.getCameraInformation().camera_model;
		if (!is_zed_camera_control_supported(*descriptor, camera_model) || !can_read_camera_control_locked(*descriptor)) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}
		if (request.min_value > request.max_value) {
			return cvmmap::unexpected(-ERANGE);
		}

		const auto result = camera.setCameraSettings(
			descriptor->zed_setting,
			request.min_value,
			request.max_value);
		if (result != sl::ERROR_CODE::SUCCESS) {
			return cvmmap::unexpected(map_camera_control_operation_error(result, -ERANGE));
		}

		return get_camera_control_locked(*descriptor);
	}


	cvmmap::expected<recording_status_t, error_t> StartRecording(const svo_recording_request_t &request) {
		if (is_svo_stream_mode(options.zed_config.stream_mode)) {
			set_recording_error("recording not supported for SVO playback input");
			return cvmmap::unexpected(-EOPNOTSUPP);
		}

		if (request.output_path.empty()) {
			set_recording_error("recording path is empty");
			return cvmmap::unexpected(-EINVAL);
		}
		if (request.output_path.find('\0') != std::string_view::npos) {
			set_recording_error("recording path contains embedded NUL");
			return cvmmap::unexpected(-EINVAL);
		}

		clear_recording_error();

		std::optional<std::string> error_message;
		std::optional<error_t> error_code;
		std::optional<recording_status_t> status;
		{
			std::lock_guard lock(camera_mutex);
			if (!initialized.load(std::memory_order_relaxed) || !camera.isOpened()) {
				error_message = "ZED camera is not opened";
				error_code = -ENODEV;
			} else {
				const auto current_status = camera.getRecordingStatus();
				if (current_status.is_recording) {
					error_message = "recording is already active";
					error_code = -EBUSY;
				} else {
					const auto output_path_fs = std::filesystem::path(request.output_path);
					const auto parent_dir = output_path_fs.parent_path();
					if (!parent_dir.empty()) {
						std::error_code ec;
						std::filesystem::create_directories(parent_dir, ec);
						if (ec) {
							error_message = cvmmap::format(
								"failed to create recording directory '{}': {}",
								parent_dir.string(),
								ec.message());
							error_code = -EIO;
						}
					}

					if (!error_message) {
						sl::RecordingParameters recording_parameters{};
						recording_parameters.video_filename = request.output_path;
						try {
							recording_parameters.compression_mode =
								parse_recording_compression_mode(
									request.options.compression_mode.value_or(
										options.zed_config.recording.compression_mode));
							recording_parameters.bitrate =
								request.options.bitrate.value_or(options.zed_config.recording.bitrate);
							recording_parameters.target_framerate =
								request.options.target_framerate.value_or(
									options.zed_config.recording.target_framerate);
							recording_parameters.transcode_streaming_input =
								request.options.transcode_streaming_input.value_or(
									options.zed_config.recording.transcode_streaming_input);
						} catch (const std::invalid_argument &e) {
							error_message = cvmmap::format(
								"invalid SVO recording options: {}",
								e.what());
							error_code = -EINVAL;
						} catch (const std::exception &e) {
							error_message = cvmmap::format(
								"failed to prepare ZED recording parameters: {}",
								e.what());
							error_code = -EIO;
						}

						if (!error_message) {
							const auto recording_result = camera.enableRecording(recording_parameters);
							if (recording_result != sl::ERROR_CODE::SUCCESS) {
								const std::string code_name = sl::toString(recording_result).get();
								const std::string verbose = sl::toVerbose(recording_result).get();
								if (verbose.empty() || verbose == code_name) {
									error_message = cvmmap::format(
										"ZED recording failed: {} ({})",
										code_name,
										static_cast<int>(recording_result));
								} else {
									error_message = cvmmap::format(
										"ZED recording failed: {} ({}): {}",
										code_name,
										static_cast<int>(recording_result),
										verbose);
								}
								error_code = -EIO;
							} else {
								active_recording_path = request.output_path;
								status = make_recording_status_locked();
							}
						}
					}
				}
			}
		}

		if (error_message) {
			set_recording_error(*error_message);
			spdlog::error("failed to start ZED recording '{}': {}", request.output_path, *error_message);
			return cvmmap::unexpected(*error_code);
		}

		clear_recording_error();
		return *status;
	}

	cvmmap::expected<recording_status_t, error_t> StopRecording() {
		std::lock_guard lock(camera_mutex);
		if (!camera.isOpened()) {
			recording_status_t status{};
			status.format = cvmmap::RecordingFormat::Svo;
			status.can_record = true;
			return status;
		}

		stop_recording_locked("control stop");
		return make_recording_status_locked();
	}

	cvmmap::expected<recording_status_t, error_t> GetRecordingStatus() {
		std::lock_guard lock(camera_mutex);
		return make_recording_status_locked();
	}
};

ZedBackend::ZedBackend(const app::ZedConfig &zed_config, const app::VideoConfig &video_config)
	: impl(std::make_unique<ZedBackendImpl>(ZedBackendOptions{
		  .zed_config   = zed_config,
		  .video_config = video_config,
	  })) {}

ZedBackend::~ZedBackend() = default;

void ZedBackend::Init() {
	impl->Init();
}

void ZedBackend::Shutdown() {
	impl->Shutdown();
}

void ZedBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void ZedBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

void ZedBackend::SetOnFrameDirect(on_zed_frame_direct_fn_t on_frame_direct) {
	impl->SetOnFrameDirect(std::move(on_frame_direct));
}

void ZedBackend::SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking) {
	impl->SetOnBodyTracking(std::move(on_body_tracking));
}

void ZedBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

source_info_t ZedBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
}

cvmmap::expected<seek_result_t, error_t> ZedBackend::SeekTimestampNs(uint64_t timestamp_ns) {
	return impl->SeekTimestampNs(timestamp_ns);
}

error_t ZedBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

camera_control_capabilities_t ZedBackend::GetCameraControlCapabilities() {
	return impl->GetCameraControlCapabilities();
}

cvmmap::expected<camera_control_state_t, error_t> ZedBackend::GetCameraControl(
	cvmmap::CameraControlSetting setting) {
	return impl->GetCameraControl(setting);
}

cvmmap::expected<camera_control_state_t, error_t> ZedBackend::SetCameraControl(
	const camera_control_request_t &request) {
	return impl->SetCameraControl(request);
}

cvmmap::expected<camera_control_state_t, error_t> ZedBackend::SetCameraControlRange(
	const camera_control_range_request_t &request) {
	return impl->SetCameraControlRange(request);
}

cvmmap::expected<recording_status_t, error_t> ZedBackend::StartRecording(
	const svo_recording_request_t &request) {
	return impl->StartRecording(request);
}

cvmmap::expected<recording_status_t, error_t> ZedBackend::StopRecording() {
	return impl->StopRecording();
}

cvmmap::expected<recording_status_t, error_t> ZedBackend::GetRecordingStatus() {
	return impl->GetRecordingStatus();
}

std::string ZedBackend::GetLastRecordingError() {
	return impl->GetLastRecordingError();
}

cvmmap::expected<uint64_t, std::string> ProbeZedSvoStartTimestampNs(
	const app::ZedConfig &zed_config) {
	if (!zed_config.svo_path || zed_config.svo_path->empty()) {
		return cvmmap::unexpected("zed.svo_path is not configured");
	}
	return probe_zed_svo_start_timestamp_ns(*zed_config.svo_path);
}

#else

struct ZedBackendImpl {
	on_metadata_fn_t _on_metadata{nullptr};
	on_frame_fn_t _on_frame{nullptr};
	on_body_tracking_fn_t _on_body_tracking{nullptr};
	on_error_fn_t _on_error{nullptr};
	std::string last_recording_error{"ZED SDK not available in this build environment"};

	void Init() {
		spdlog::error("ZED SDK headers not found at compile time");
		if (_on_error) {
			_on_error(-ENODEV, "ZED SDK not available in this build environment");
		}
	}

	void Shutdown() {}

	void SetOnMetadata(on_metadata_fn_t on_metadata_) {
		_on_metadata = std::move(on_metadata_);
	}

	void SetOnFrame(on_frame_fn_t on_frame_) {
		_on_frame = std::move(on_frame_);
	}

	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking_) {
		_on_body_tracking = std::move(on_body_tracking_);
	}

	void SetOnError(on_error_fn_t on_error_) {
		_on_error = std::move(on_error_);
	}

	void SetOnFrameDirect(on_zed_frame_direct_fn_t on_frame_direct_) {
		(void)on_frame_direct_;
	}

	source_info_t GetSourceInfo() {
		source_info_t info{};
		info.source_kind = cvmmap::SourceKind::Live;
		info.timestamp_domain = cvmmap::TimestampDomain::UnixEpochNs;
		return info;
	}

	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	error_t ResetFrameCount() {
		return 0;
	}

 	camera_control_capabilities_t GetCameraControlCapabilities() {
 		return {};
 	}

 	cvmmap::expected<camera_control_state_t, error_t> GetCameraControl(cvmmap::CameraControlSetting) {
 		return cvmmap::unexpected(-EOPNOTSUPP);
 	}

 	cvmmap::expected<camera_control_state_t, error_t> SetCameraControl(const camera_control_request_t &) {
 		return cvmmap::unexpected(-EOPNOTSUPP);
 	}

 	cvmmap::expected<camera_control_state_t, error_t> SetCameraControlRange(
 		const camera_control_range_request_t &) {
 		return cvmmap::unexpected(-EOPNOTSUPP);
 	}

	cvmmap::expected<recording_status_t, error_t> StartRecording(const svo_recording_request_t &) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	cvmmap::expected<recording_status_t, error_t> StopRecording() {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	cvmmap::expected<recording_status_t, error_t> GetRecordingStatus() {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	std::string GetLastRecordingError() {
		return last_recording_error;
	}
};

ZedBackend::ZedBackend(const app::ZedConfig &, const app::VideoConfig &)
	: impl(std::make_unique<ZedBackendImpl>()) {}

ZedBackend::~ZedBackend() = default;

void ZedBackend::Init() {
	impl->Init();
}

void ZedBackend::Shutdown() {
	impl->Shutdown();
}

void ZedBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void ZedBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

void ZedBackend::SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking) {
	impl->SetOnBodyTracking(std::move(on_body_tracking));
}

void ZedBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

void ZedBackend::SetOnFrameDirect(on_zed_frame_direct_fn_t on_frame_direct) {
	impl->SetOnFrameDirect(std::move(on_frame_direct));
}

source_info_t ZedBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
}

cvmmap::expected<seek_result_t, error_t> ZedBackend::SeekTimestampNs(uint64_t timestamp_ns) {
	return impl->SeekTimestampNs(timestamp_ns);
}

error_t ZedBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

camera_control_capabilities_t ZedBackend::GetCameraControlCapabilities() {
	return impl->GetCameraControlCapabilities();
}

cvmmap::expected<camera_control_state_t, error_t> ZedBackend::GetCameraControl(
	cvmmap::CameraControlSetting setting) {
	return impl->GetCameraControl(setting);
}

cvmmap::expected<camera_control_state_t, error_t> ZedBackend::SetCameraControl(
	const camera_control_request_t &request) {
	return impl->SetCameraControl(request);
}

cvmmap::expected<camera_control_state_t, error_t> ZedBackend::SetCameraControlRange(
	const camera_control_range_request_t &request) {
	return impl->SetCameraControlRange(request);
}

cvmmap::expected<recording_status_t, error_t> ZedBackend::StartRecording(
	const svo_recording_request_t &request) {
	return impl->StartRecording(request);
}

cvmmap::expected<recording_status_t, error_t> ZedBackend::StopRecording() {
	return impl->StopRecording();
}

cvmmap::expected<recording_status_t, error_t> ZedBackend::GetRecordingStatus() {
	return impl->GetRecordingStatus();
}

std::string ZedBackend::GetLastRecordingError() {
	return impl->GetLastRecordingError();
}

cvmmap::expected<uint64_t, std::string> ProbeZedSvoStartTimestampNs(
	const app::ZedConfig &) {
	return cvmmap::unexpected("ZED SDK not available in this build environment");
}

#endif

}
