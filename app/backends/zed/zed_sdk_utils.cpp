#include "zed_sdk_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include <cvmmap/compat/format.hpp>

namespace app::backends {
namespace {

std::string normalize_ascii_lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

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

void copy_uint2_box(
	std::array<std::array<float, 2>, cvmmap::BODY_BOX2D_POINTS> &dst,
	const std::vector<sl::uint2> &src) {
	for (size_t i = 0; i < cvmmap::BODY_BOX2D_POINTS; ++i) {
		dst[i][0] = nan32();
		dst[i][1] = nan32();
	}
	for (size_t i = 0; i < std::min(src.size(), static_cast<size_t>(cvmmap::BODY_BOX2D_POINTS)); ++i) {
		dst[i][0] = static_cast<float>(src[i].x);
		dst[i][1] = static_cast<float>(src[i].y);
	}
}

void copy_float3_box(
	std::array<std::array<float, 3>, cvmmap::BODY_BOX3D_POINTS> &dst,
	const std::vector<sl::float3> &src) {
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

} // namespace

bool is_network_stream_mode(const std::string_view mode) {
	const auto normalized = normalize_ascii_lower(std::string(mode));
	return normalized == "network" || normalized == "ethernet" || normalized == "stream";
}

bool is_svo_stream_mode(const std::string_view mode) {
	return normalize_ascii_lower(std::string(mode)) == "svo";
}

const ZedCameraControlDescriptor *find_zed_camera_control_descriptor(
	const cvmmap::CameraControlSetting setting) {
	for (const auto &descriptor : ZED_CAMERA_CONTROL_DESCRIPTORS) {
		if (descriptor.setting == setting) {
			return &descriptor;
		}
	}
	return nullptr;
}

bool is_zed_camera_control_supported(
	const ZedCameraControlDescriptor &descriptor,
	const sl::MODEL model) {
	return is_zed_x_family_model(model) ? descriptor.x_family_supported : descriptor.legacy_supported;
}

std::span<const ZedCameraControlDescriptor> zed_camera_control_descriptors() {
	return std::span<const ZedCameraControlDescriptor>(
		ZED_CAMERA_CONTROL_DESCRIPTORS.data(),
		ZED_CAMERA_CONTROL_DESCRIPTORS.size());
}

uint8_t channels_from_mat_type(const sl::MAT_TYPE type) {
	switch (type) {
	case sl::MAT_TYPE::F32_C1:
	case sl::MAT_TYPE::U8_C1:
	case sl::MAT_TYPE::U16_C1:
		return 1;
	case sl::MAT_TYPE::F32_C2:
	case sl::MAT_TYPE::U8_C2:
		return 2;
	case sl::MAT_TYPE::F32_C3:
	case sl::MAT_TYPE::U8_C3:
		return 3;
	case sl::MAT_TYPE::F32_C4:
	case sl::MAT_TYPE::U8_C4:
	case sl::MAT_TYPE::S8_C4:
		return 4;
	case sl::MAT_TYPE::NV12:
	default:
		return 0;
	}
}

std::optional<Depth> depth_from_mat_type(const sl::MAT_TYPE type) {
	switch (type) {
	case sl::MAT_TYPE::F32_C1:
	case sl::MAT_TYPE::F32_C2:
	case sl::MAT_TYPE::F32_C3:
	case sl::MAT_TYPE::F32_C4:
		return Depth::F32;
	case sl::MAT_TYPE::U8_C1:
	case sl::MAT_TYPE::U8_C2:
	case sl::MAT_TYPE::U8_C3:
	case sl::MAT_TYPE::U8_C4:
		return Depth::U8;
	case sl::MAT_TYPE::U16_C1:
		return Depth::U16;
	case sl::MAT_TYPE::S8_C4:
		return Depth::S8;
	case sl::MAT_TYPE::NV12:
	default:
		return std::nullopt;
	}
}

PixelFormat guess_pixel_format_for_zed(const sl::MAT_TYPE type) {
	switch (type) {
	case sl::MAT_TYPE::U8_C1:
	case sl::MAT_TYPE::U16_C1:
	case sl::MAT_TYPE::F32_C1:
		return PixelFormat::GRAY;
	case sl::MAT_TYPE::U8_C3:
		return PixelFormat::BGR;
	case sl::MAT_TYPE::U8_C4:
	case sl::MAT_TYPE::S8_C4:
		return PixelFormat::BGRA;
	case sl::MAT_TYPE::F32_C3:
		return PixelFormat::RGB;
	case sl::MAT_TYPE::F32_C4:
		return PixelFormat::RGBA;
	case sl::MAT_TYPE::U8_C2:
	case sl::MAT_TYPE::F32_C2:
	case sl::MAT_TYPE::NV12:
	default:
		throw std::invalid_argument(cvmmap::format(
			"unsupported ZED MAT_TYPE pixel-format mapping: {}",
			static_cast<int>(type)));
	}
}

sl::RESOLUTION parse_resolution(const std::string &value) {
	const auto normalized = normalize_ascii_lower(value);
	if (normalized == "hd2k") return sl::RESOLUTION::HD2K;
	if (normalized == "hd1200") return sl::RESOLUTION::HD1200;
	if (normalized == "hd1080") return sl::RESOLUTION::HD1080;
	if (normalized == "hd720") return sl::RESOLUTION::HD720;
	if (normalized == "svga") return sl::RESOLUTION::SVGA;
	if (normalized == "vga") return sl::RESOLUTION::VGA;
	if (normalized == "auto") return sl::RESOLUTION::AUTO;
	if (normalized == "2k") return sl::RESOLUTION::HD2K;
	if (normalized == "1080p" || normalized == "fhd") return sl::RESOLUTION::HD1080;
	if (normalized == "720p" || normalized == "hd") return sl::RESOLUTION::HD720;
	throw std::invalid_argument("unknown ZED resolution: " + value + "; valid values: hd2k|hd1200|hd1080|hd720|svga|vga|auto or aliases: 2k|1080p|fhd|720p|hd");
}

sl::DEPTH_MODE parse_depth_mode(const std::string &value) {
	auto normalized = normalize_ascii_lower(value);
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');
	if (normalized == "none") return sl::DEPTH_MODE::NONE;
	if (normalized == "neural_light") return sl::DEPTH_MODE::NEURAL_LIGHT;
	if (normalized == "neural") return sl::DEPTH_MODE::NEURAL;
	if (normalized == "neural_plus") return sl::DEPTH_MODE::NEURAL_PLUS;
	throw std::invalid_argument(
		"unknown ZED depth_mode: " + value +
		"; valid values: none|neural|neural_light|neural_plus");
}

sl::SVO_COMPRESSION_MODE parse_recording_compression_mode(const std::string &value) {
	auto normalized = normalize_ascii_lower(value);
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');
	if (normalized == "lossless") return sl::SVO_COMPRESSION_MODE::LOSSLESS;
	if (normalized == "h264") return sl::SVO_COMPRESSION_MODE::H264;
	if (normalized == "h265") return sl::SVO_COMPRESSION_MODE::H265;
	if (normalized == "h264_lossless") return sl::SVO_COMPRESSION_MODE::H264_LOSSLESS;
	if (normalized == "h265_lossless") return sl::SVO_COMPRESSION_MODE::H265_LOSSLESS;
	throw std::invalid_argument(
		"unknown ZED recording compression_mode: " + value +
		"; valid values: lossless|h264|h265|h264_lossless|h265_lossless");
}

sl::VIEW parse_left_view(std::string value) {
	value = normalize_ascii_lower(std::move(value));
	if (value == "gray8" || value == "mono8") return sl::VIEW::LEFT_GRAY;
	if (value == "bgr8" || value == "rgb8") return sl::VIEW::LEFT_BGR;
	return sl::VIEW::LEFT_BGRA;
}

sl::BODY_TRACKING_MODEL parse_body_tracking_model(const std::string &value) {
	const auto normalized = normalize_ascii_lower(value);
	if (normalized == "human_body_fast") return sl::BODY_TRACKING_MODEL::HUMAN_BODY_FAST;
	if (normalized == "human_body_medium") return sl::BODY_TRACKING_MODEL::HUMAN_BODY_MEDIUM;
	return sl::BODY_TRACKING_MODEL::HUMAN_BODY_ACCURATE;
}

sl::BODY_FORMAT parse_body_format(const std::string &value) {
	const auto normalized = normalize_ascii_lower(value);
	if (normalized == "body_34") return sl::BODY_FORMAT::BODY_34;
	if (normalized == "body_38") return sl::BODY_FORMAT::BODY_38;
	return sl::BODY_FORMAT::BODY_18;
}

sl::BODY_KEYPOINTS_SELECTION parse_body_selection(const std::string &value) {
	const auto normalized = normalize_ascii_lower(value);
	if (normalized == "upper_body") return sl::BODY_KEYPOINTS_SELECTION::UPPER_BODY;
	return sl::BODY_KEYPOINTS_SELECTION::FULL;
}

sl::COORDINATE_SYSTEM parse_coordinate_system(const std::string &value) {
	auto normalized = normalize_ascii_lower(value);
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');
	if (normalized == "image") return sl::COORDINATE_SYSTEM::IMAGE;
	if (normalized == "right_handed_y_up") return sl::COORDINATE_SYSTEM::RIGHT_HANDED_Y_UP;
	throw std::invalid_argument(
		"unsupported ZED coordinate_system: " + value +
		"; supported values: IMAGE|RIGHT_HANDED_Y_UP");
}

sl::REFERENCE_FRAME parse_body_reference_frame(const std::string &value) {
	auto normalized = normalize_ascii_lower(value);
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');
	if (normalized == "camera") return sl::REFERENCE_FRAME::CAMERA;
	if (normalized == "world") return sl::REFERENCE_FRAME::WORLD;
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

cvmmap::BodyKeypointSelection to_body_selection(
	const sl::BODY_KEYPOINTS_SELECTION value) {
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

void copy_body_record(
	cvmmap::body_tracking_body_t &record,
	const sl::BodyData &body) {
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
		copy_float3(record.keypoint_3d[i], body.keypoint[i]);
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
		copy_float3(record.local_position_per_joint[i], body.local_position_per_joint[i]);
	}
	for (size_t i = 0; i < std::min(body.local_orientation_per_joint.size(), static_cast<size_t>(cvmmap::BODY_KEYPOINT_CAPACITY)); ++i) {
		copy_float4(record.local_orientation_per_joint[i], body.local_orientation_per_joint[i]);
	}
	copy_float4(record.global_root_orientation, body.global_root_orientation);
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
			"ZED SVO open error: {}",
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
			"initial ZED SVO grab error: {}",
			sl::toString(grab_result).get()));
	}

	const auto timestamp_ns = zed_image_timestamp_ns(camera);
	camera.close();
	if (timestamp_ns == 0) {
		return cvmmap::unexpected("ZED SVO file returned zero image timestamp");
	}
	return timestamp_ns;
}

} // namespace app::backends
