#include <errno.h>
#include <memory>
#include <utility>

#if defined(WITH_BACKEND_ZED) && __has_include(<sl/Camera.hpp>)
#include <sl/Camera.hpp>
#define APP_HAS_ZED_SDK 1
#else
#define APP_HAS_ZED_SDK 0
#endif

#include <cvmmap/compat/expected.hpp>
#include <spdlog/spdlog.h>

#include "app_backends_facade.hpp"
#include "app_backends_zed.hpp"

#if APP_HAS_ZED_SDK

#include "zed_backend_internal.hpp"
#endif

namespace app::backends {

#if APP_HAS_ZED_SDK

ZedBackendImpl::ZedBackendImpl() = default;

ZedBackendImpl::ZedBackendImpl(ZedBackendOptions opts)
	: options(std::move(opts)) {}

ZedBackendImpl::~ZedBackendImpl() {
	Shutdown();
}

void ZedBackendImpl::on_metadata(const frame_metadata_t &m) {
	if (_on_metadata) {
		_on_metadata(m);
	}
}

void ZedBackendImpl::on_frame(
	std::span<uint8_t> frame_buffer,
	const frame_metadata_t &m) {
	if (_on_frame) {
		_on_frame(frame_buffer, m);
	}
}

void ZedBackendImpl::on_direct_frame(direct_frame_t frame) {
	if (_on_direct_frame) {
		_on_direct_frame(std::move(frame));
	}
}

void ZedBackendImpl::on_error(
	const error_t error_code,
	const std::string_view message) {
	if (_on_error) {
		_on_error(error_code, message);
	}
}

void ZedBackendImpl::on_body_tracking(
	const cvmmap::body_tracking_frame_t &frame) {
	if (_on_body_tracking) {
		_on_body_tracking(frame);
	}
}


void ZedBackendImpl::SetOnDirectFrame(
	on_direct_frame_fn_t on_direct_frame_) {
	_on_direct_frame = std::move(on_direct_frame_);
}

void ZedBackendImpl::Shutdown() {
	stop_worker_thread();

	std::lock_guard lock(camera_mutex);
	if (camera.isOpened()) {
		stop_recording_locked("backend shutdown");
		camera.close();
	}

	initialized.store(false, std::memory_order::relaxed);
}

void ZedBackendImpl::SetOnMetadata(on_metadata_fn_t on_metadata_) {
	_on_metadata = std::move(on_metadata_);
}

void ZedBackendImpl::SetOnFrame(on_frame_fn_t on_frame_) {
	_on_frame = std::move(on_frame_);
}

void ZedBackendImpl::SetOnBodyTracking(
	on_body_tracking_fn_t on_body_tracking_) {
	_on_body_tracking = std::move(on_body_tracking_);
}

void ZedBackendImpl::SetOnError(on_error_fn_t on_error_) {
	_on_error = std::move(on_error_);
}

source_info_t ZedBackendImpl::GetSourceInfo() {
	std::lock_guard lock(snapshot_mutex);
	return published.source_info;
}

ZedBackend::ZedBackend(
	const app::ZedConfig &zed_config,
	const app::VideoConfig &video_config)
	: impl(std::make_unique<ZedBackendImpl>(ZedBackendOptions{
		  .zed_config = zed_config,
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

void ZedBackend::SetOnDirectFrame(on_direct_frame_fn_t on_frame_direct) {
	impl->SetOnDirectFrame(std::move(on_frame_direct));
}

void ZedBackend::OnDirectOutputBufferWillReset(
	std::span<const uint8_t> output_buffer) {
	impl->OnDirectOutputBufferWillReset(output_buffer);
}

void ZedBackend::SetOnBodyTracking(
	on_body_tracking_fn_t on_body_tracking) {
	impl->SetOnBodyTracking(std::move(on_body_tracking));
}

void ZedBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

source_info_t ZedBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
}

error_t ZedBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

camera_control_capabilities_t ZedBackend::GetCameraControlCapabilities() {
	return impl->GetCameraControlCapabilities();
}

cvmmap::expected<camera_control_state_t, error_t> ZedBackend::GetCameraControl(
	const cvmmap::CameraControlSetting setting) {
	return impl->GetCameraControl(setting);
}

cvmmap::expected<camera_control_state_t, error_t> ZedBackend::SetCameraControl(
	const camera_control_request_t &request) {
	return impl->SetCameraControl(request);
}

cvmmap::expected<camera_control_state_t, error_t>
ZedBackend::SetCameraControlRange(
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
	std::string last_recording_error{
		"ZED SDK not available in this build environment"};

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

	void SetOnDirectFrame(on_direct_frame_fn_t on_frame_direct_) {
		(void)on_frame_direct_;
	}

	void OnDirectOutputBufferWillReset(std::span<const uint8_t>) {}

	source_info_t GetSourceInfo() {
		source_info_t info{};
		info.source_kind = cvmmap::SourceKind::Live;
		info.timestamp_domain = cvmmap::TimestampDomain::UnixEpochNs;
		return info;
	}

	error_t ResetFrameCount() {
		return 0;
	}

	camera_control_capabilities_t GetCameraControlCapabilities() {
		return {};
	}

	cvmmap::expected<camera_control_state_t, error_t> GetCameraControl(
		cvmmap::CameraControlSetting) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	cvmmap::expected<camera_control_state_t, error_t> SetCameraControl(
		const camera_control_request_t &) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	cvmmap::expected<camera_control_state_t, error_t> SetCameraControlRange(
		const camera_control_range_request_t &) {
		return cvmmap::unexpected(-EOPNOTSUPP);
	}

	cvmmap::expected<recording_status_t, error_t> StartRecording(
		const svo_recording_request_t &) {
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

void ZedBackend::SetOnDirectFrame(on_direct_frame_fn_t on_frame_direct) {
	impl->SetOnDirectFrame(std::move(on_frame_direct));
}

void ZedBackend::OnDirectOutputBufferWillReset(
	std::span<const uint8_t> output_buffer) {
	impl->OnDirectOutputBufferWillReset(output_buffer);
}

source_info_t ZedBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
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

cvmmap::expected<camera_control_state_t, error_t>
ZedBackend::SetCameraControlRange(
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

} // namespace app::backends
