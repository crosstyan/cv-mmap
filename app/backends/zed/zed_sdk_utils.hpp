#pragma once

#include "app_enum_models.hpp"
#include <cvmmap/backend_types.hpp>
#include <cvmmap/compat/expected.hpp>
#include <cvmmap/ipc.hpp>

#include <sl/Camera.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace app::backends {

struct ZedCameraControlDescriptor {
	cvmmap::CameraControlSetting setting;
	cvmmap::CameraControlValueKind kind;
	sl::VIDEO_SETTINGS zed_setting;
	bool legacy_supported;
	bool x_family_supported;
};

bool is_network_stream_mode(std::string_view mode);
bool is_svo_stream_mode(std::string_view mode);

const ZedCameraControlDescriptor *find_zed_camera_control_descriptor(
	cvmmap::CameraControlSetting setting);
bool is_zed_camera_control_supported(
	const ZedCameraControlDescriptor &descriptor,
	sl::MODEL model);
std::span<const ZedCameraControlDescriptor>
zed_camera_control_descriptors();

uint8_t channels_from_mat_type(sl::MAT_TYPE type);
std::optional<Depth> depth_from_mat_type(sl::MAT_TYPE type);
PixelFormat guess_pixel_format_for_zed(sl::MAT_TYPE type);
sl::RESOLUTION parse_resolution(const std::string &value);
sl::DEPTH_MODE parse_depth_mode(const std::string &value);
sl::SVO_COMPRESSION_MODE parse_recording_compression_mode(
	const std::string &value);
sl::VIEW parse_left_view(std::string value);
sl::BODY_TRACKING_MODEL parse_body_tracking_model(const std::string &value);
sl::BODY_FORMAT parse_body_format(const std::string &value);
sl::BODY_KEYPOINTS_SELECTION parse_body_selection(const std::string &value);
sl::COORDINATE_SYSTEM parse_coordinate_system(const std::string &value);
sl::REFERENCE_FRAME parse_body_reference_frame(const std::string &value);

cvmmap::BodyTrackingModel to_body_tracking_model(sl::BODY_TRACKING_MODEL value);
cvmmap::BodyFormat to_body_format(sl::BODY_FORMAT value);
cvmmap::BodyKeypointSelection to_body_selection(
	sl::BODY_KEYPOINTS_SELECTION value);
cvmmap::BodyCoordinateSystem to_body_coordinate_system(
	sl::COORDINATE_SYSTEM value);
cvmmap::BodyReferenceFrame to_body_reference_frame(
	sl::REFERENCE_FRAME value);
cvmmap::InferencePrecision to_inference_precision(
	sl::INFERENCE_PRECISION value);
cvmmap::ObjectTrackingState to_tracking_state(
	sl::OBJECT_TRACKING_STATE value);
cvmmap::ObjectActionState to_action_state(sl::OBJECT_ACTION_STATE value);

void copy_body_record(
	cvmmap::body_tracking_body_t &record,
	const sl::BodyData &body);

uint64_t now_ns();
uint64_t zed_image_timestamp_ns(sl::Camera &camera);
cvmmap::expected<uint64_t, std::string> probe_zed_svo_start_timestamp_ns(
	const std::string &path);

} // namespace app::backends
