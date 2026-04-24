#include "backend_runtime.hpp"

#include <cerrno>
#include <utility>

#include <spdlog/spdlog.h>

namespace app {
namespace {

cvmmap::ControlErrorCode map_control_error_code(const int error_code) {
	switch (error_code) {
	case 0:
		return cvmmap::ControlErrorCode::Ok;
	case -EOPNOTSUPP:
		return cvmmap::ControlErrorCode::Unsupported;
	case -EINVAL:
		return cvmmap::ControlErrorCode::InvalidPayload;
	case -ERANGE:
		return cvmmap::ControlErrorCode::OutOfRange;
	default:
		return cvmmap::ControlErrorCode::Error;
	}
}

cvmmap::ControlError map_recording_error(
	const int error_code,
	std::string message = {}) {
	return cvmmap::ControlError{
		.code = map_control_error_code(error_code),
		.message = std::move(message),
	};
}

cvmmap::CameraControlState to_public_camera_control_state(
	const backends::camera_control_state_t &state) {
	return cvmmap::CameraControlState{
		.setting = state.setting,
		.kind = state.kind,
		.value = state.value,
		.min_value = state.min_value,
		.max_value = state.max_value,
	};
}

cvmmap::CameraControlCapabilities to_public_camera_control_capabilities(
	const backends::camera_control_capabilities_t &capabilities) {
	return cvmmap::CameraControlCapabilities{
		.supported = capabilities.supported,
		.supported_settings = capabilities.supported_settings,
	};
}

cvmmap::ControlError map_backend_control_error(
	const int error_code,
	std::string message = {}) {
	return cvmmap::ControlError{
		.code = map_control_error_code(error_code),
		.message = std::move(message),
	};
}

cvmmap::SvoRecordingStatus to_public_recording_status(
	const backends::recording_status_t &status) {
	return cvmmap::SvoRecordingStatus{
		.can_record = status.can_record,
		.is_recording = status.is_recording,
		.is_paused = status.is_paused,
		.last_frame_ok = status.last_frame_ok,
		.frames_ingested = status.frames_ingested,
		.frames_encoded = status.frames_encoded,
		.active_path = status.active_path,
	};
}

} // namespace

BackendRuntime::BackendRuntime(
	Config &config,
	FramePublisher &frame_publisher,
	BodyTrackingPublisher &body_tracking_publisher)
	: config_(&config),
	  frame_publisher_(&frame_publisher),
	  body_tracking_publisher_(&body_tracking_publisher) {}

bool BackendRuntime::InitializeActiveBackend(BackendRuntimeCallbacks callbacks) {
	callbacks_ = std::move(callbacks);

	auto created_assembly = backends::MakeBackendAssembly(*config_);
	if (!created_assembly) {
		spdlog::error("{}", created_assembly.error());
		return false;
	}

	assembly_ = std::move(*created_assembly);
	frame_publisher_->Reset();
	frame_publisher_->Configure(FramePublisherOptions{
		.uses_direct_frame = assembly_.direct_frame.has_value(),
	});

	if (assembly_.encoded_access_unit) {
		auto encoded_access_unit = *assembly_.encoded_access_unit;
		encoded_access_unit->SetOnEncodedAccessUnit(
			[this](const backends::encoded_access_unit_t &access_unit) {
				frame_publisher_->OnEncodedAccessUnit(access_unit);
			});
	}

	BindBackendCallbacks();
	RefreshCameraControlProvider();
	RefreshSvoRecorderProvider();
	assembly_.backend->Init();
	return true;
}

void BackendRuntime::ShutdownActiveBackend() {
	if (assembly_.backend) {
		assembly_.backend->Shutdown();
	}
}

void BackendRuntime::ResetFrameState() {
	frame_publisher_->Reset();
}

backends::BackendAssembly &BackendRuntime::assembly() {
	return assembly_;
}

const backends::BackendAssembly &BackendRuntime::assembly() const {
	return assembly_;
}

const std::optional<CameraControlProvider> &
BackendRuntime::camera_control_provider() const {
	return camera_control_provider_;
}

const std::optional<SvoRecorderProvider> &
BackendRuntime::svo_recorder_provider() const {
	return svo_recorder_provider_;
}

ActiveBackendSourceSnapshot BackendRuntime::SnapshotSourcePath() const {
	return config_->SnapshotActiveBackendSource();
}

void BackendRuntime::RestoreSourcePath(
	const ActiveBackendSourceSnapshot &snapshot) {
	config_->RestoreActiveBackendSource(snapshot);
}

void BackendRuntime::RefreshCameraControlProvider() {
	camera_control_provider_.reset();
	if (!assembly_.camera_control) {
		return;
	}

	auto capability = *assembly_.camera_control;
	camera_control_provider_ = CameraControlProvider{
		.capabilities = [capability = std::move(capability)]() mutable
			-> cvmmap::expected<cvmmap::CameraControlCapabilities, cvmmap::ControlError> {
			return to_public_camera_control_capabilities(
				capability->GetCameraControlCapabilities());
		},
		.get = [capability = *assembly_.camera_control](
				const cvmmap::CameraControlSetting setting) mutable
			-> cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError> {
			auto result = capability->GetCameraControl(setting);
			if (!result) {
				return cvmmap::unexpected(
					map_backend_control_error(result.error()));
			}
			return to_public_camera_control_state(*result);
		},
		.set = [capability = *assembly_.camera_control](
				const cvmmap::CameraControlRequest &request) mutable
			-> cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError> {
			backends::camera_control_request_t backend_request{
				.setting = request.setting,
				.mode = request.mode,
				.value = request.value,
			};
			auto result = capability->SetCameraControl(backend_request);
			if (!result) {
				return cvmmap::unexpected(
					map_backend_control_error(result.error()));
			}
			return to_public_camera_control_state(*result);
		},
		.set_range = [capability = *assembly_.camera_control](
				const cvmmap::CameraControlRangeRequest &request) mutable
			-> cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError> {
			backends::camera_control_range_request_t backend_request{
				.setting = request.setting,
				.min_value = request.min_value,
				.max_value = request.max_value,
			};
			auto result = capability->SetCameraControlRange(backend_request);
			if (!result) {
				return cvmmap::unexpected(
					map_backend_control_error(result.error()));
			}
			return to_public_camera_control_state(*result);
		},
	};
}

void BackendRuntime::RefreshSvoRecorderProvider() {
	svo_recorder_provider_.reset();
	if (!assembly_.svo_recordable) {
		return;
	}

	auto capability = *assembly_.svo_recordable;
	svo_recorder_provider_ = SvoRecorderProvider{
		.is_available = [capability = *assembly_.svo_recordable]() mutable {
			auto status = capability->GetRecordingStatus();
			return status && status->can_record;
		},
		.start = [capability = *assembly_.svo_recordable](
				 const cvmmap::SvoRecordingRequest &request) mutable
			-> cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError> {
			backends::svo_recording_request_t backend_request{
				.output_path = request.output_path,
			};
			if (request.svo_options) {
				backend_request.options.compression_mode = request.svo_options->compression_mode;
				backend_request.options.bitrate = request.svo_options->bitrate;
				backend_request.options.target_framerate = request.svo_options->target_framerate;
				backend_request.options.transcode_streaming_input =
					request.svo_options->transcode_streaming_input;
			}
			auto result = capability->StartRecording(backend_request);
			if (!result) {
				return cvmmap::unexpected(
					map_recording_error(
						result.error(),
						capability->GetLastRecordingError()));
			}
			return to_public_recording_status(*result);
		},
		.stop = [capability = *assembly_.svo_recordable]() mutable
			-> cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError> {
			auto result = capability->StopRecording();
			if (!result) {
				return cvmmap::unexpected(
					map_recording_error(
						result.error(),
						capability->GetLastRecordingError()));
			}
			return to_public_recording_status(*result);
		},
		.status = [capability = std::move(capability)]() mutable
			-> cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError> {
			auto result = capability->GetRecordingStatus();
			if (!result) {
				return cvmmap::unexpected(
					map_recording_error(
						result.error(),
						capability->GetLastRecordingError()));
			}
			return to_public_recording_status(*result);
		},
	};
}

void BackendRuntime::BindBackendCallbacks() {
	auto backend = assembly_.backend;
	backend->SetOnMetadata(
		[this, direct_frame = assembly_.direct_frame](
			const frame_metadata_t &metadata) mutable {
			frame_publisher_->OnMetadata(
				metadata,
				direct_frame
					? FramePublisher::direct_buffer_reset_hook_t{
						  [direct_frame = *direct_frame](
							  std::span<const uint8_t> buffer) mutable {
							  direct_frame->OnDirectOutputBufferWillReset(buffer);
						  }}
					: FramePublisher::direct_buffer_reset_hook_t{});
		});

	if (assembly_.direct_frame) {
		auto direct_frame = *assembly_.direct_frame;
		auto direct_frame_callback = direct_frame;
		direct_frame->SetOnDirectFrame(
			[this, direct_frame = std::move(direct_frame_callback)](
				backends::direct_frame_t frame) mutable {
				frame_publisher_->PublishDirectFrame(
					std::move(frame),
					[direct_frame](std::span<const uint8_t> buffer) mutable {
						direct_frame->OnDirectOutputBufferWillReset(buffer);
					});
			});
	} else {
		backend->SetOnFrame(
			[this](
				std::span<uint8_t> frame_buffer,
				const frame_metadata_t &metadata) {
				frame_publisher_->PublishFrame(frame_buffer, metadata);
			});
	}

	if (assembly_.body_tracking) {
		auto body_tracking = *assembly_.body_tracking;
		body_tracking->SetOnBodyTracking(
			[this](const cvmmap::body_tracking_frame_t &frame) {
				body_tracking_publisher_->Publish(frame);
			});
	}

	backend->SetOnError(
		[this, backend](int error_code, std::string_view message) mutable {
			if (error_code == backends::ERR_EOS) {
				spdlog::info("backend EOF: {}", message);
				if (callbacks_.request_playlist_item_transition &&
					callbacks_.request_playlist_item_transition(false)) {
					return;
				}

				const auto source_info = backend->GetSourceInfo();
				if ((source_info.flags & cvmmap::SOURCE_INFO_FLAG_AUTO_LOOP) != 0) {
					spdlog::info("looping finite stream (encore)");
					if (callbacks_.request_playlist_transition) {
						callbacks_.request_playlist_transition(
							(source_info.flags & cvmmap::SOURCE_INFO_FLAG_LOOP_EMITS_RESET) != 0
								? PlaylistTransitionAction::ResetActiveEmitReset
								: PlaylistTransitionAction::ResetActiveSilent);
					}
					return;
				}
			} else if (error_code == backends::ERR_SKIP_PLAYLIST_ITEM) {
				if (callbacks_.request_playlist_item_transition &&
					callbacks_.request_playlist_item_transition(true)) {
					spdlog::warn("skipping bad finite source item: {}", message);
					return;
				}
				spdlog::error("finite source item is invalid: {}", message);
			} else {
				if (error_code == backends::ERR_FATAL_CAMERA_RECOVERY &&
					callbacks_.mark_fatal_camera_recovery) {
					callbacks_.mark_fatal_camera_recovery();
				}
				spdlog::error("backend error {}: {}", error_code, message);
			}

			if (callbacks_.stop_running) {
				callbacks_.stop_running();
			}
		});
}

} // namespace app
