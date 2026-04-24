#include "zed_backend_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include <cvmmap/compat/format.hpp>
#include <spdlog/spdlog.h>

namespace app::backends {

void ZedBackendImpl::EthernetExtensionStubs::log_placeholder_status() const {
	spdlog::debug(
		"ZED ethernet extension stubs active (non-functional placeholders)");
}

void ZedBackendImpl::invalidate_direct_depth_tracking_locked() {
	depth_fallback.invalidate_direct_tracking();
}

void ZedBackendImpl::clear_direct_output_bindings_locked() {
	direct_output_binding = {};
	invalidate_direct_depth_tracking_locked();
}

void ZedBackendImpl::reset_depth_fallback_locked() {
	depth_fallback.reset();
	clear_direct_output_bindings_locked();
}

void ZedBackendImpl::reset_depth_publish_cadence_locked() {
	depth_cadence.reset(depth_enabled);
	runtime_parameters.enable_depth = depth_enabled;
}

void ZedBackendImpl::configure_depth_publish_cadence_locked() {
	const auto reported_source_fps =
		camera.getCameraInformation().camera_configuration.fps;
	const auto cadence = depth_cadence.configure(ZedDepthCadenceConfig{
		.depth_enabled = depth_enabled,
		.reported_source_fps = reported_source_fps,
		.configured_source_fps = static_cast<double>(options.zed_config.fps),
		.depth_max_fps = options.zed_config.depth_max_fps,
	});
	runtime_parameters.enable_depth = depth_enabled;

	if (!cadence.depth_enabled) {
		spdlog::info("ZED depth cadence disabled (depth_mode=NONE)");
		return;
	}

	if (cadence.fps_source == ZedDepthFpsSource::Configured) {
		spdlog::warn(
			"ZED reported invalid source FPS {}; falling back to configured FPS {} for depth cadence",
			reported_source_fps,
			options.zed_config.fps);
	} else if (
		cadence.fps_source == ZedDepthFpsSource::DeterministicFallback) {
		spdlog::warn(
			"ZED reported invalid source FPS {}; falling back to 1 FPS for deterministic depth cadence",
			reported_source_fps);
	}

	spdlog::info(
		"ZED depth cadence configured: depthMaxFps={} sourceFps={} periodFrames={} depthMode={}",
		cadence.configured_depth_max_fps,
		cadence.effective_source_fps,
		cadence.period_frames,
		options.zed_config.depth_mode);
}

bool ZedBackendImpl::should_request_depth_for_grab_locked(
	const bool force_depth_request) const {
	return depth_cadence.should_request(force_depth_request);
}

void ZedBackendImpl::commit_depth_request_for_grab_locked(
	const bool depth_requested) {
	if (!depth_enabled) {
		runtime_parameters.enable_depth = false;
		return;
	}
	depth_cadence.commit(depth_requested);
}

void ZedBackendImpl::snapshot_last_direct_depth_plane_locked(
	std::span<const uint8_t> output_buffer) {
	if (!depth_enabled) {
		invalidate_direct_depth_tracking_locked();
		return;
	}
	(void)depth_fallback.snapshot_tracked_direct_plane(output_buffer);
}

void ZedBackendImpl::OnDirectOutputBufferWillReset(
	std::span<const uint8_t> output_buffer) {
	std::lock_guard lock(camera_mutex);
	snapshot_last_direct_depth_plane_locked(output_buffer);
	clear_direct_output_bindings_locked();
}

std::string ZedBackendImpl::format_svo_skip_message(
	const std::string_view prefix) const {
	const auto path = options.zed_config.svo_path.value_or("<unknown>");
	if (last_grab_error != sl::ERROR_CODE::SUCCESS) {
		return cvmmap::format(
			"{} '{}': {}",
			prefix,
			path,
			sl::toString(last_grab_error).get());
	}
	return cvmmap::format("{} '{}'", prefix, path);
}

uint64_t ZedBackendImpl::effective_timestamp_ns_locked() {
	return svo_mode ? zed_image_timestamp_ns(camera) : now_ns();
}

uint32_t ZedBackendImpl::next_frame_count() const {
	std::lock_guard lock(snapshot_mutex);
	return published.metadata.frame_count + 1;
}

std::chrono::milliseconds ZedBackendImpl::publish_gap_warning_threshold() const {
	const auto fps = std::max(1, options.zed_config.fps);
	const auto expected_publish_ms = std::max(1, (1000 / fps) * 5);
	return std::max(
		std::chrono::milliseconds(2000),
		std::chrono::milliseconds(expected_publish_ms));
}

bool ZedBackendImpl::uses_live_camera_recovery() const {
	return !svo_mode;
}

std::optional<std::string> ZedBackendImpl::fatal_capture_error_message(
	const sl::ERROR_CODE code) const {
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

source_info_t ZedBackendImpl::make_source_info_locked() const {
	source_info_t info{};
	info.source_kind =
		svo_mode ? cvmmap::SourceKind::Finite : cvmmap::SourceKind::Live;
	info.timestamp_domain = cvmmap::TimestampDomain::UnixEpochNs;
	info.flags |= cvmmap::SOURCE_INFO_FLAG_HAS_DEPTH;
	if (!svo_mode) {
		info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_RECORD;
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
		info.duration_ns = timeline_end_ns >= timeline_start_ns
			? timeline_end_ns - timeline_start_ns
			: 0;
	}
	return info;
}

bool ZedBackendImpl::initialize_svo_timeline_locked() {
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

	auto sample_timestamp_ns =
		[this](const int position) -> std::optional<uint64_t> {
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

std::optional<cvmmap::body_tracking_frame_t>
ZedBackendImpl::capture_body_tracking_frame_locked(
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
		spdlog::warn(
			"ZED retrieveBodies error: code={}",
			static_cast<int>(retrieve_result));
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
	frame.header.body_selection =
		to_body_selection(body_tracking_parameters.body_selection);
	frame.header.detection_model =
		to_body_tracking_model(body_tracking_parameters.detection_model);
	frame.header.inference_precision =
		to_inference_precision(bodies.inference_precision_mode);
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
		frame.header.flags |= 
			cvmmap::BODY_TRACKING_FLAG_BODY_FITTING_ENABLED;
	}
	if (body_tracking_parameters.allow_reduced_precision_inference) {
		frame.header.flags |= 
			cvmmap::BODY_TRACKING_FLAG_REDUCED_PRECISION_REQUESTED;
	}
	std::memset(frame.header._label, 0, sizeof(frame.header._label));
	std::memcpy(
		frame.header._label,
		options.zed_config.body_tracking
			? options.zed_config.body_tracking->body_format.c_str()
			: "",
		std::min(
			sizeof(frame.header._label),
			options.zed_config.body_tracking
				? options.zed_config.body_tracking->body_format.size()
				: static_cast<size_t>(0)));

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
	frame.header.payload_size_bytes = static_cast<uint32_t>(
		frame.bodies.size() * sizeof(cvmmap::body_tracking_body_t));
	return frame;
}

cvmmap::expected<ZedBackendImpl::CapturedFrame, sl::ERROR_CODE>
ZedBackendImpl::capture_frame_locked(
	const uint32_t frame_count,
	const bool force_depth_request) {
	const bool depth_requested =
		should_request_depth_for_grab_locked(force_depth_request);
	runtime_parameters.enable_depth = depth_requested;
	const auto grab_result = camera.grab(runtime_parameters);
	last_grab_error = grab_result;
	if (grab_result != sl::ERROR_CODE::SUCCESS) {
		spdlog::debug("ZED grab error: code={}", static_cast<int>(grab_result));
		return cvmmap::unexpected(grab_result);
	}
	last_grab_error = sl::ERROR_CODE::SUCCESS;
	commit_depth_request_for_grab_locked(depth_requested);

	std::optional<frame_info_t> info;
	if (_on_direct_frame) {
		info = make_direct_frame_info_locked();
		if (!info) {
			spdlog::error("invalid ZED frame geometry/type for direct SHM packing");
			return cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
		}
	} else {
		const auto retrieve_result =
			camera.retrieveImage(left_frame, left_view, sl::MEM::CPU);
		if (retrieve_result != sl::ERROR_CODE::SUCCESS) {
			spdlog::debug(
				"ZED retrieveImage error: code={}",
				static_cast<int>(retrieve_result));
			return cvmmap::unexpected(retrieve_result);
		}

		info = make_frame_info(left_frame);
		if (!info) {
			spdlog::error("invalid left frame geometry/type for compact packing");
			return cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
		}
	}

	CapturedFrame captured{};
	captured.info = *info;
	captured.depth_requested = depth_requested;
	const size_t depth_plane_bytes = static_cast<size_t>(captured.info.width) *
		static_cast<size_t>(captured.info.height) * sizeof(float);
	size_t raw_capacity = captured.info.buffer_size;
	if (depth_requested) {
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
		captured.body_tracking =
			capture_body_tracking_frame_locked(frame_count, captured.timestamp_ns);
	}
	return captured;
}

ZedBackendImpl::PublishedFrame ZedBackendImpl::publish_captured_frame(
	CapturedFrame captured,
	std::vector<uint8_t> payload,
	frame_payload_layout_t layout,
	const uint32_t frame_count,
	const bool log_publish_gap) {
	PublishedFrame published_frame{};
	published_frame.metadata.frame_count = frame_count;
	published_frame.metadata.timestamp_ns = captured.timestamp_ns;
	published_frame.metadata.info = captured.info;
	published_frame.metadata.info.buffer_size =
		static_cast<uint32_t>(payload.size());
	published_frame.payload = std::move(payload);
	published_frame.layout = std::move(layout);
	published_frame.body_tracking = std::move(captured.body_tracking);

	std::optional<int64_t> publish_gap_ms;
	{
		std::lock_guard lock(snapshot_mutex);
		published.metadata = published_frame.metadata;
		published.source_info = captured.source_info;
		published.source_info.current_timestamp_ns =
			published_frame.metadata.timestamp_ns;
		published.source_info.current_frame_count =
			published_frame.metadata.frame_count;
		if (log_publish_gap && published.has_last_publish_at) {
			const auto gap =
				std::chrono::steady_clock::now() - published.last_publish_at;
			if (gap > publish_gap_warning_threshold()) {
				publish_gap_ms =
					std::chrono::duration_cast<std::chrono::milliseconds>(gap)
						.count();
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

ZedBackendImpl::DirectPublishedFrame
ZedBackendImpl::publish_captured_frame_direct(
	CapturedFrame captured,
	const uint32_t frame_count,
	const bool log_publish_gap) {
	DirectPublishedFrame published_frame{};
	published_frame.metadata.frame_count = frame_count;
	published_frame.metadata.timestamp_ns = captured.timestamp_ns;
	published_frame.metadata.info = captured.info;
	published_frame.body_tracking = std::move(captured.body_tracking);
	published_frame.fill_payload =
		[this, info = published_frame.metadata.info, depth_requested = captured.depth_requested](
			std::span<uint8_t> output_buffer) mutable -> std::optional<direct_frame_fill_result_t> {
		std::lock_guard lock(camera_mutex);
		return pack_frame_locked(output_buffer, info, true, depth_requested);
	};

	std::optional<int64_t> publish_gap_ms;
	{
		std::lock_guard lock(snapshot_mutex);
		published.metadata = published_frame.metadata;
		published.source_info = captured.source_info;
		published.source_info.current_timestamp_ns =
			published_frame.metadata.timestamp_ns;
		published.source_info.current_frame_count =
			published_frame.metadata.frame_count;
		if (log_publish_gap && published.has_last_publish_at) {
			const auto gap =
				std::chrono::steady_clock::now() - published.last_publish_at;
			if (gap > publish_gap_warning_threshold()) {
				publish_gap_ms =
					std::chrono::duration_cast<std::chrono::milliseconds>(gap)
						.count();
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

void ZedBackendImpl::emit_published_frame(PublishedFrame &published_frame) {
	on_frame(
		std::span<uint8_t>(
			published_frame.payload.data(),
			published_frame.payload.size()),
		published_frame.metadata,
		published_frame.layout);
	if (published_frame.body_tracking) {
		on_body_tracking(*published_frame.body_tracking);
	}
}

void ZedBackendImpl::emit_published_frame_direct(
	DirectPublishedFrame published_frame) {
	on_direct_frame(direct_frame_t{
		.metadata = published_frame.metadata,
		.fill_payload = std::move(published_frame.fill_payload),
	});
	if (published_frame.body_tracking) {
		on_body_tracking(*published_frame.body_tracking);
	}
}

error_t ZedBackendImpl::ResetFrameCount() {
	if (!is_svo_stream_mode(options.zed_config.stream_mode)) {
		std::lock_guard lock(snapshot_mutex);
		published.metadata.frame_count = 0;
		published.source_info.current_frame_count = 0;
		return 0;
	}

	stop_worker_thread();

	cvmmap::expected<CapturedFrame, sl::ERROR_CODE> capture =
		cvmmap::unexpected(sl::ERROR_CODE::FAILURE);
	bool restart_worker = false;
	{
		std::lock_guard lock(camera_mutex);
		restart_worker = initialized.load(std::memory_order_relaxed) &&
			camera.isOpened() && total_svo_frames > 0;
		if (!restart_worker) {
			return -ENODEV;
		}

		camera.setSVOPosition(0);
		reset_depth_fallback_locked();
		capture = capture_frame_locked(0, true);
	}

	if (!capture) {
		return capture.error() == sl::ERROR_CODE::END_OF_SVOFILE_REACHED
			? -ERANGE
			: -EIO;
	}

	auto published_frame = publish_captured_frame_direct(
		std::move(*capture),
		0,
		false);
	if (_on_direct_frame) {
		emit_published_frame_direct(std::move(published_frame));
	} else {
		std::vector<uint8_t> payload(
			published_frame.metadata.info.buffer_size);
		std::optional<direct_frame_fill_result_t> packed_frame;
		{
			std::lock_guard lock(camera_mutex);
			packed_frame = pack_frame_locked(
				std::span<uint8_t>(payload.data(), payload.size()),
				published_frame.metadata.info,
				false,
				capture->depth_requested);
		}
		if (!packed_frame) {
			return -EIO;
		}
		payload.resize(packed_frame->payload_size_bytes);
		auto fallback = publish_captured_frame(
			CapturedFrame{
				.info = published_frame.metadata.info,
				.source_info = make_source_info_locked(),
				.payload = {},
				.body_tracking = std::move(published_frame.body_tracking),
				.timestamp_ns = published_frame.metadata.timestamp_ns,
			},
			std::move(payload),
			std::move(packed_frame->layout),
			0,
			false);
		emit_published_frame(fallback);
	}
	if (restart_worker) {
		start_worker_thread();
	}
	return ERR_OK;
}

} // namespace app::backends
