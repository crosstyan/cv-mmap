#include "zed_backend_internal.hpp"

namespace app::backends {

error_t ZedBackendImpl::camera_control_availability_error_locked() const {
	if (svo_mode) {
		return -EOPNOTSUPP;
	}
	if (!initialized.load(std::memory_order_relaxed) || !camera.isOpened()) {
		return -ENODEV;
	}
	return ERR_OK;
}

error_t ZedBackendImpl::map_camera_control_operation_error(
	const sl::ERROR_CODE code,
	const error_t invalid_parameters_error) {
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

bool ZedBackendImpl::can_read_camera_control_locked(
	const ZedCameraControlDescriptor &descriptor) {
	if (descriptor.kind == cvmmap::CameraControlValueKind::Single) {
		int value = 0;
		return camera.getCameraSettings(descriptor.zed_setting, value) ==
			sl::ERROR_CODE::SUCCESS;
	}

	int min_value = 0;
	int max_value = 0;
	return camera.getCameraSettings(
			   descriptor.zed_setting,
			   min_value,
			   max_value) == sl::ERROR_CODE::SUCCESS;
}

cvmmap::expected<camera_control_state_t, error_t>
ZedBackendImpl::get_camera_control_locked(
	const ZedCameraControlDescriptor &descriptor) {
	camera_control_state_t state{};
	state.setting = descriptor.setting;
	state.kind = descriptor.kind;

	if (descriptor.kind == cvmmap::CameraControlValueKind::Single) {
		int value = 0;
		const auto result =
			camera.getCameraSettings(descriptor.zed_setting, value);
		if (result != sl::ERROR_CODE::SUCCESS) {
			return cvmmap::unexpected(
				map_camera_control_operation_error(result, -EIO));
		}
		state.value = value;
		return state;
	}

	int min_value = 0;
	int max_value = 0;
	const auto result = camera.getCameraSettings(
		descriptor.zed_setting,
		min_value,
		max_value);
	if (result != sl::ERROR_CODE::SUCCESS) {
		return cvmmap::unexpected(
			map_camera_control_operation_error(result, -EIO));
	}
	state.min_value = min_value;
	state.max_value = max_value;
	return state;
}

camera_control_capabilities_t ZedBackendImpl::GetCameraControlCapabilities() {
	std::lock_guard lock(camera_mutex);
	camera_control_capabilities_t capabilities{};
	if (camera_control_availability_error_locked() != ERR_OK) {
		return capabilities;
	}

	const auto camera_model = camera.getCameraInformation().camera_model;
	for (const auto &descriptor : zed_camera_control_descriptors()) {
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

cvmmap::expected<camera_control_state_t, error_t>
ZedBackendImpl::GetCameraControl(const cvmmap::CameraControlSetting setting) {
	std::lock_guard lock(camera_mutex);
	if (const auto availability_error = camera_control_availability_error_locked();
		availability_error != ERR_OK) {
		return cvmmap::unexpected(availability_error);
	}

	const auto *descriptor = find_zed_camera_control_descriptor(setting);
	if (descriptor == nullptr) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	const auto camera_model = camera.getCameraInformation().camera_model;
	if (!is_zed_camera_control_supported(*descriptor, camera_model) ||
		!can_read_camera_control_locked(*descriptor)) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	return get_camera_control_locked(*descriptor);
}

cvmmap::expected<camera_control_state_t, error_t>
ZedBackendImpl::SetCameraControl(const camera_control_request_t &request) {
	std::lock_guard lock(camera_mutex);
	if (const auto availability_error = camera_control_availability_error_locked();
		availability_error != ERR_OK) {
		return cvmmap::unexpected(availability_error);
	}

	const auto *descriptor = find_zed_camera_control_descriptor(request.setting);
	if (descriptor == nullptr ||
		descriptor->kind != cvmmap::CameraControlValueKind::Single) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	const auto camera_model = camera.getCameraInformation().camera_model;
	if (!is_zed_camera_control_supported(*descriptor, camera_model) ||
		!can_read_camera_control_locked(*descriptor)) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	if (request.mode != cvmmap::CameraControlWriteMode::Manual &&
		request.mode != cvmmap::CameraControlWriteMode::Auto) {
		return cvmmap::unexpected(-EINVAL);
	}

	const int value = request.mode == cvmmap::CameraControlWriteMode::Auto
		? sl::VIDEO_SETTINGS_VALUE_AUTO
		: request.value;
	const auto result = camera.setCameraSettings(descriptor->zed_setting, value);
	if (result != sl::ERROR_CODE::SUCCESS) {
		return cvmmap::unexpected(
			map_camera_control_operation_error(result, -ERANGE));
	}

	return get_camera_control_locked(*descriptor);
}

cvmmap::expected<camera_control_state_t, error_t>
ZedBackendImpl::SetCameraControlRange(
	const camera_control_range_request_t &request) {
	std::lock_guard lock(camera_mutex);
	if (const auto availability_error = camera_control_availability_error_locked();
		availability_error != ERR_OK) {
		return cvmmap::unexpected(availability_error);
	}

	const auto *descriptor = find_zed_camera_control_descriptor(request.setting);
	if (descriptor == nullptr ||
		descriptor->kind != cvmmap::CameraControlValueKind::Range) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	const auto camera_model = camera.getCameraInformation().camera_model;
	if (!is_zed_camera_control_supported(*descriptor, camera_model) ||
		!can_read_camera_control_locked(*descriptor)) {
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
		return cvmmap::unexpected(
			map_camera_control_operation_error(result, -ERANGE));
	}

	return get_camera_control_locked(*descriptor);
}

} // namespace app::backends
