#include "zed_backend_internal.hpp"

#include <filesystem>
#include <system_error>

#include <cvmmap/compat/format.hpp>
#include <spdlog/spdlog.h>

namespace app::backends {

void ZedBackendImpl::clear_recording_error() {
	std::lock_guard lock(snapshot_mutex);
	published.last_recording_error.clear();
}

void ZedBackendImpl::set_recording_error(std::string message) {
	std::lock_guard lock(snapshot_mutex);
	published.last_recording_error = std::move(message);
}

std::string ZedBackendImpl::GetLastRecordingError() {
	std::lock_guard lock(snapshot_mutex);
	return published.last_recording_error;
}

recording_status_t ZedBackendImpl::make_recording_status_locked() {
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
	status.frames_ingested = sdk_status.number_frames_ingested < 0
		? 0u
		: static_cast<uint32_t>(sdk_status.number_frames_ingested);
	status.frames_encoded = sdk_status.number_frames_encoded < 0
		? 0u
		: static_cast<uint32_t>(sdk_status.number_frames_encoded);

	if (status.is_recording) {
		if (!active_recording_path.empty()) {
			status.active_path = active_recording_path;
		} else {
			status.active_path = camera.getRecordingParameters().video_filename.get();
		}
	}

	return status;
}

void ZedBackendImpl::stop_recording_locked(std::string_view reason) {
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

cvmmap::expected<recording_status_t, error_t> ZedBackendImpl::StartRecording(
	const svo_recording_request_t &request) {
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
							"recording directory create error '{}': {}",
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
						recording_parameters.bitrate = request.options.bitrate.value_or(
							options.zed_config.recording.bitrate);
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
							"bad ZED recording parameters: {}",
							e.what());
						error_code = -EIO;
					}

					if (!error_message) {
						const auto recording_result =
							camera.enableRecording(recording_parameters);
						if (recording_result != sl::ERROR_CODE::SUCCESS) {
							const std::string code_name =
								sl::toString(recording_result).get();
							const std::string verbose =
								sl::toVerbose(recording_result).get();
							if (verbose.empty() || verbose == code_name) {
								error_message = cvmmap::format(
									"ZED recording error: {} ({})",
									code_name,
									static_cast<int>(recording_result));
							} else {
								error_message = cvmmap::format(
									"ZED recording error: {} ({}): {}",
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
		spdlog::error(
			"ZED recording start error for '{}': {}",
			request.output_path,
			*error_message);
		return cvmmap::unexpected(*error_code);
	}

	clear_recording_error();
	return *status;
}

cvmmap::expected<recording_status_t, error_t> ZedBackendImpl::StopRecording() {
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

cvmmap::expected<recording_status_t, error_t>
ZedBackendImpl::GetRecordingStatus() {
	std::lock_guard lock(camera_mutex);
	return make_recording_status_locked();
}

} // namespace app::backends
