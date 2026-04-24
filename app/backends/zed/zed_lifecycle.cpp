#include <errno.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/format.hpp>
#include <spdlog/spdlog.h>

#include "zed_backend_internal.hpp"

namespace app::backends {

void ZedBackendImpl::stop_worker_thread() {
	if (worker_thread.joinable()) {
		worker_thread.request_stop();
		worker_thread.join();
	}
}

void ZedBackendImpl::start_worker_thread() {
	worker_thread = std::jthread([this](std::stop_token stop_token) {
		worker_loop(stop_token);
	});
}

bool ZedBackendImpl::open_camera_locked() {
	init_parameters = sl::InitParameters{};
	init_parameters.camera_resolution =
		parse_resolution(options.zed_config.resolution);
	init_parameters.camera_fps = options.zed_config.fps;
	init_parameters.depth_mode =
		parse_depth_mode(options.zed_config.depth_mode);
	init_parameters.depth_stabilization =
		options.zed_config.depth_stabilization;
	depth_enabled = init_parameters.depth_mode != sl::DEPTH_MODE::NONE;
	body_tracking_enabled = options.zed_config.body_tracking.has_value() &&
		options.zed_config.body_tracking->enabled;
	svo_mode = is_svo_stream_mode(options.zed_config.stream_mode);
	if (!svo_mode) {
		init_parameters.async_grab_camera_recovery = true;
		init_parameters.enable_image_validity_check = 1;
	}

	if (body_tracking_enabled && options.zed_config.depth_max_fps > 0) {
		spdlog::error(
			"ZED body tracking requires depth on every frame; zed.depth_max_fps={} is unsupported",
			options.zed_config.depth_max_fps);
		return false;
	}

	if (body_tracking_enabled) {
		init_parameters.coordinate_units = sl::UNIT::METER;
		init_parameters.coordinate_system =
			parse_coordinate_system(options.zed_config.coordinate_system);
		body_reference_frame = parse_body_reference_frame(
			options.zed_config.body_tracking->reference_frame);
		runtime_parameters.measure3D_reference_frame = body_reference_frame;
	}

	const auto stream_mode = options.zed_config.stream_mode;

	if (svo_mode) {
		if (!options.zed_config.svo_path || options.zed_config.svo_path->empty()) {
			spdlog::error("ZED stream_mode='svo' requires svo_path");
			return false;
		}
		init_parameters.svo_real_time_mode =
			options.zed_config.svo_real_time_mode;
		spdlog::info(
			"opening ZED SVO playback '{}'",
			*options.zed_config.svo_path);
		init_parameters.input.setFromSVOFile(
			sl::String(options.zed_config.svo_path->c_str()));
	} else if (is_network_stream_mode(stream_mode)) {
		eth_stubs.log_placeholder_status();

		if (!options.zed_config.ip_address ||
			options.zed_config.ip_address->empty()) {
			spdlog::error(
				"ZED stream_mode='{}' requires ip_address",
				options.zed_config.stream_mode);
			return false;
		}

		if (options.zed_config.port) {
			spdlog::info(
				"[NETWORK MODE: {}] opening ZED network stream {}:{}",
				options.zed_config.stream_mode,
				*options.zed_config.ip_address,
				*options.zed_config.port);
			init_parameters.input.setFromStream(
				sl::String(options.zed_config.ip_address->c_str()),
				static_cast<unsigned short>(*options.zed_config.port));
		} else {
			spdlog::info(
				"[NETWORK MODE: {}] opening ZED network stream {} (default port)",
				options.zed_config.stream_mode,
				*options.zed_config.ip_address);
			init_parameters.input.setFromStream(
				sl::String(options.zed_config.ip_address->c_str()));
		}
	} else {
		if (options.zed_config.serial) {
			spdlog::info(
				"[LOCAL MODE: {}] opening ZED local camera by serial={}",
				options.zed_config.stream_mode,
				*options.zed_config.serial);
			init_parameters.input.setFromSerialNumber(*options.zed_config.serial);
		} else if (options.zed_config.index) {
			spdlog::info(
				"[LOCAL MODE: {}] opening ZED local camera by index={}",
				options.zed_config.stream_mode,
				*options.zed_config.index);
			init_parameters.input.setFromCameraID(*options.zed_config.index);
		} else {
			spdlog::info(
				"[LOCAL MODE: {}] opening ZED local camera by default selection",
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
				positional_tracking_parameters =
					sl::PositionalTrackingParameters{};
				positional_tracking_parameters.set_floor_as_origin =
					options.zed_config.body_tracking->set_floor_as_origin;
				auto positional_result = camera.enablePositionalTracking(
					positional_tracking_parameters);
				if (positional_result != sl::ERROR_CODE::SUCCESS) {
					spdlog::error(
						"bad ZED positional tracking setup: code={}",
						static_cast<int>(positional_result));
					camera.close();
					return false;
				}

				const auto &cfg = *options.zed_config.body_tracking;
				body_tracking_parameters = sl::BodyTrackingParameters{};
				body_tracking_parameters.enable_tracking = true;
				body_tracking_parameters.enable_segmentation = false;
				body_tracking_parameters.detection_model =
					parse_body_tracking_model(cfg.detection_model);
				body_tracking_parameters.enable_body_fitting =
					cfg.enable_body_fitting;
				body_tracking_parameters.body_format =
					parse_body_format(cfg.body_format);
				body_tracking_parameters.body_selection =
					parse_body_selection(cfg.body_selection);
				body_tracking_parameters.max_range = cfg.max_range;
				body_tracking_parameters.prediction_timeout_s =
					cfg.prediction_timeout_s;
				body_tracking_parameters.allow_reduced_precision_inference =
					cfg.allow_reduced_precision_inference;

				body_tracking_runtime_parameters =
					sl::BodyTrackingRuntimeParameters{};
				body_tracking_runtime_parameters.detection_confidence_threshold =
					cfg.detection_confidence_threshold;
				body_tracking_runtime_parameters.minimum_keypoints_threshold =
					cfg.minimum_keypoints_threshold;
				body_tracking_runtime_parameters.skeleton_smoothing =
					cfg.skeleton_smoothing;

				auto body_tracking_result =
					camera.enableBodyTracking(body_tracking_parameters);
				if (body_tracking_result != sl::ERROR_CODE::SUCCESS) {
					spdlog::error(
						"bad ZED body tracking setup: code={}",
						static_cast<int>(body_tracking_result));
					camera.disablePositionalTracking();
					camera.close();
					return false;
				}
			}
			spdlog::info("ZED camera opened");
			configure_depth_publish_cadence_locked();
			if (!initialize_svo_timeline_locked()) {
				if (camera.isOpened()) {
					camera.close();
				}
				spdlog::error("bad ZED SVO timeline initialization");
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

	spdlog::error(
		"bad ZED camera open after {}ms timeout: code={}",
		timeout_ms,
		static_cast<int>(open_result));
	return false;
}

void ZedBackendImpl::Init() {
	if (initialized.load(std::memory_order::relaxed)) {
		return;
	}

	cvmmap::expected<CapturedFrame, sl::ERROR_CODE> initial_capture =
		cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
	{
		std::lock_guard lock(camera_mutex);
		if (!open_camera_locked()) {
			if (svo_mode) {
				on_error(
					ERR_SKIP_PLAYLIST_ITEM,
					format_svo_skip_message("ZED SVO open error"));
			} else {
				on_error(-ENODEV, "ZED camera open error");
			}
			return;
		}

		initial_capture = capture_frame_locked(0, true);
		if (!initial_capture) {
			spdlog::error("bad first-frame capture from ZED");
			if (svo_mode) {
				on_error(
					ERR_SKIP_PLAYLIST_ITEM,
					format_svo_skip_message(
						"bad first-frame read from ZED SVO"));
			} else if (const auto fatal_capture_message =
						   fatal_capture_error_message(initial_capture.error());
					   fatal_capture_message) {
				on_error(ERR_FATAL_CAMERA_RECOVERY, *fatal_capture_message);
			} else {
				const auto message = cvmmap::format(
					"bad first-frame capture: {}",
					sl::toString(initial_capture.error()).get());
				on_error(-EIO, message);
			}
			camera.close();
			return;
		}
	}

	auto initial_direct =
		publish_captured_frame_direct(std::move(*initial_capture), 0, false);

	spdlog::info(
		"initial ZED frame info: {}x{}x{}; depth={}; bufferSize={}; pixelFormat={}; depthPlaneEnabled={}",
		initial_direct.metadata.info.width,
		initial_direct.metadata.info.height,
		initial_direct.metadata.info.channels,
		app::to_str(initial_direct.metadata.info.depth),
		initial_direct.metadata.info.buffer_size,
		app::to_str(initial_direct.metadata.info.pixel_format),
		depth_enabled);

	on_metadata(initial_direct.metadata);
	if (_on_direct_frame) {
		emit_published_frame_direct(std::move(initial_direct));
	} else {
		std::vector<uint8_t> payload(initial_direct.metadata.info.buffer_size);
		std::optional<direct_frame_fill_result_t> packed_frame;
		{
			std::lock_guard lock(camera_mutex);
			packed_frame = pack_frame_locked(
				std::span<uint8_t>(payload.data(), payload.size()),
				initial_direct.metadata.info,
				false,
				initial_capture->depth_requested);
		}
		if (!packed_frame) {
			spdlog::error("bad initial ZED frame payload pack");
			return;
		}
		payload.resize(packed_frame->payload_size_bytes);
		auto initial_published = publish_captured_frame(
			CapturedFrame{
				.info = initial_direct.metadata.info,
				.source_info = initial_capture->source_info,
				.payload = {},
				.body_tracking = std::move(initial_direct.body_tracking),
				.timestamp_ns = initial_direct.metadata.timestamp_ns,
			},
			std::move(payload),
			std::move(packed_frame->layout),
			0,
			false);
		emit_published_frame(initial_published);
	}

	initialized.store(true, std::memory_order::relaxed);
	start_worker_thread();
}

void ZedBackendImpl::worker_loop(std::stop_token stop_token) {
	const auto max_failures =
		std::max(1, options.zed_config.max_consecutive_failures);
	const auto finite_source = is_svo_stream_mode(options.zed_config.stream_mode);
	int consecutive_failures = 0;

	while (!stop_token.stop_requested()) {
		const auto frame_count = next_frame_count();
		cvmmap::expected<CapturedFrame, sl::ERROR_CODE> capture =
			cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
		{
			std::lock_guard lock(camera_mutex);
			capture = capture_frame_locked(frame_count);
		}

		if (capture) {
			consecutive_failures = 0;
			if (_on_direct_frame) {
				auto published_frame = publish_captured_frame_direct(
					std::move(*capture),
					frame_count,
					true);
				emit_published_frame_direct(std::move(published_frame));
				spdlog::debug("frame@{}", frame_count);
			} else {
				std::vector<uint8_t> payload(capture->info.buffer_size);
				auto packed_frame = pack_frame_locked(
					std::span<uint8_t>(payload.data(), payload.size()),
					capture->info,
					false,
					capture->depth_requested);
				if (!packed_frame) {
					spdlog::error("bad ZED frame payload pack");
					continue;
				}
				payload.resize(packed_frame->payload_size_bytes);
				auto published_frame = publish_captured_frame(
					std::move(*capture),
					std::move(payload),
					std::move(packed_frame->layout),
					frame_count,
					true);
				emit_published_frame(published_frame);
				spdlog::debug(
					"frame@{}",
					published_frame.metadata.frame_count);
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
		if (finite_source &&
			capture.error() == sl::ERROR_CODE::END_OF_SVOFILE_REACHED) {
			on_error(ERR_EOS, "EOF");
			break;
		}
		if (consecutive_failures < max_failures) {
			continue;
		}

		spdlog::error(
			"bad ZED capture streak: {} consecutive attempts",
			consecutive_failures);
		if (svo_mode) {
			const auto message = format_svo_skip_message(
				"corrupted or unreadable ZED SVO segment");
			on_error(ERR_SKIP_PLAYLIST_ITEM, message);
			break;
		}
		on_error(-EIO, "ZED capture error");
		break;
	}
}

} // namespace app::backends
