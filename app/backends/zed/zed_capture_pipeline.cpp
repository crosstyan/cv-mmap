#include "zed_backend_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <cvmmap/compat/format.hpp>
#include <spdlog/spdlog.h>

namespace app::backends {

namespace {

std::optional<sl::MAT_TYPE> direct_left_mat_type_for_view(const sl::VIEW view) {
	switch (view) {
	case sl::VIEW::LEFT_GRAY:
		return sl::MAT_TYPE::U8_C1;
	case sl::VIEW::LEFT_BGR:
		return sl::MAT_TYPE::U8_C3;
	case sl::VIEW::LEFT:
	case sl::VIEW::LEFT_BGRA:
		return sl::MAT_TYPE::U8_C4;
	default:
		return std::nullopt;
	}
}

std::optional<frame_info_t> make_direct_frame_info(
	const sl::Camera &camera,
	const sl::VIEW left_view) {
	const auto resolution = camera.getCameraInformation().camera_configuration.resolution;
	const auto mat_type = direct_left_mat_type_for_view(left_view);
	if (!mat_type || resolution.width <= 0 || resolution.height <= 0 ||
		resolution.width > std::numeric_limits<uint16_t>::max() ||
		resolution.height > std::numeric_limits<uint16_t>::max()) {
		return std::nullopt;
	}

	const auto channels = channels_from_mat_type(*mat_type);
	auto depth = depth_from_mat_type(*mat_type);
	if (channels == 0 || !depth) {
		return std::nullopt;
	}

	const auto row_bytes =
		static_cast<size_t>(resolution.width) * static_cast<size_t>(channels) *
		static_cast<size_t>(size_of(*depth));
	const auto buffer_size = row_bytes * static_cast<size_t>(resolution.height);
	if (buffer_size == 0 || buffer_size > std::numeric_limits<uint32_t>::max()) {
		return std::nullopt;
	}

	return frame_info_t{
		.width = static_cast<uint16_t>(resolution.width),
		.height = static_cast<uint16_t>(resolution.height),
		.channels = channels,
		.depth = *depth,
		.pixel_format = guess_pixel_format_for_zed(*mat_type),
		.buffer_size = static_cast<uint32_t>(buffer_size),
	};
}

} // namespace


void ZedBackendImpl::EthernetExtensionStubs::log_placeholder_status() const {
	spdlog::debug(
		"ZED ethernet extension stubs active (non-functional placeholders)");
}

void ZedBackendImpl::invalidate_direct_depth_tracking_locked() {
	last_direct_depth_payload_ptr = nullptr;
	last_direct_depth_payload_size = 0;
}

void ZedBackendImpl::clear_direct_output_bindings_locked() {
	direct_output_binding = {};
	invalidate_direct_depth_tracking_locked();
}

void ZedBackendImpl::reset_depth_fallback_locked() {
	last_good_depth_plane.clear();
	clear_direct_output_bindings_locked();
}

void ZedBackendImpl::reset_depth_publish_cadence_locked() {
	frames_since_last_depth_request = 0;
	force_depth_on_next_capture = true;
	runtime_parameters.enable_depth = depth_enabled;
}

void ZedBackendImpl::configure_depth_publish_cadence_locked() {
	reset_depth_publish_cadence_locked();
	effective_source_fps = 0.0;
	depth_publish_period_frames = 1;

	if (!depth_enabled) {
		spdlog::info("ZED depth cadence disabled (depth_mode=NONE)");
		return;
	}

	const auto reported_source_fps =
		camera.getCameraInformation().camera_configuration.fps;
	if (reported_source_fps > 0) {
		effective_source_fps = static_cast<double>(reported_source_fps);
	} else if (options.zed_config.fps > 0) {
		effective_source_fps = static_cast<double>(options.zed_config.fps);
		spdlog::warn(
			"ZED reported invalid source FPS {}; falling back to configured FPS {} for depth cadence",
			reported_source_fps,
			options.zed_config.fps);
	} else {
		effective_source_fps = 1.0;
		spdlog::warn(
			"ZED reported invalid source FPS {}; falling back to 1 FPS for deterministic depth cadence",
			reported_source_fps);
	}

	const auto configured_depth_max_fps =
		std::max(0, options.zed_config.depth_max_fps);
	if (configured_depth_max_fps > 0) {
		depth_publish_period_frames = std::max<uint32_t>(
			1u,
			static_cast<uint32_t>(std::ceil(
				effective_source_fps /
				static_cast<double>(configured_depth_max_fps))));
	}

	spdlog::info(
		"ZED depth cadence configured: depthMaxFps={} sourceFps={} periodFrames={} depthMode={}",
		configured_depth_max_fps,
		effective_source_fps,
		depth_publish_period_frames,
		options.zed_config.depth_mode);
}

bool ZedBackendImpl::should_request_depth_for_grab_locked(
	const bool force_depth_request) const {
	if (!depth_enabled) {
		return false;
	}
	if (force_depth_request || force_depth_on_next_capture) {
		return true;
	}
	if (options.zed_config.depth_max_fps <= 0) {
		return true;
	}
	return frames_since_last_depth_request + 1 >= depth_publish_period_frames;
}

void ZedBackendImpl::commit_depth_request_for_grab_locked(
	const bool depth_requested) {
	if (!depth_enabled) {
		runtime_parameters.enable_depth = false;
		return;
	}
	force_depth_on_next_capture = false;
	if (depth_requested) {
		frames_since_last_depth_request = 0;
		return;
	}
	if (frames_since_last_depth_request < std::numeric_limits<uint32_t>::max()) {
		frames_since_last_depth_request += 1;
	}
}

void ZedBackendImpl::snapshot_last_direct_depth_plane_locked(
	std::span<const uint8_t> output_buffer) {
	if (!depth_enabled) {
		invalidate_direct_depth_tracking_locked();
		return;
	}
	if (last_direct_depth_payload_ptr == nullptr ||
		last_direct_depth_payload_size == 0) {
		invalidate_direct_depth_tracking_locked();
		return;
	}
	const auto *buffer_begin = output_buffer.data();
	if (buffer_begin == nullptr || output_buffer.empty()) {
		invalidate_direct_depth_tracking_locked();
		return;
	}
	const auto *buffer_end = buffer_begin + output_buffer.size();
	const auto *depth_begin = last_direct_depth_payload_ptr;
	const auto *depth_end = depth_begin + last_direct_depth_payload_size;
	if (depth_begin >= buffer_begin && depth_end <= buffer_end) {
		last_good_depth_plane.resize(last_direct_depth_payload_size);
		std::memcpy(
			last_good_depth_plane.data(),
			depth_begin,
			last_direct_depth_payload_size);
	}
	invalidate_direct_depth_tracking_locked();
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

std::optional<size_t> ZedBackendImpl::expected_row_bytes(
	const sl::Mat &frame) const {
	const auto width = frame.getWidth();
	if (width <= 0) {
		return std::nullopt;
	}

	const auto channels = channels_from_mat_type(frame.getDataType());
	auto depth = depth_from_mat_type(frame.getDataType());
	if (channels == 0 || !depth) {
		return std::nullopt;
	}

	return static_cast<size_t>(width) * static_cast<size_t>(channels) *
		static_cast<size_t>(size_of(*depth));
}

bool ZedBackendImpl::copy_compact_plane(
	const sl::Mat &src,
	const size_t row_bytes,
	std::span<uint8_t> dst) const {
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

std::optional<ZedBackendImpl::DirectOutputLayout>
ZedBackendImpl::compute_direct_output_layout_locked(
	const frame_info_t &info_out,
	const bool depth_requested) const {
	const auto left_mat_type = direct_left_mat_type_for_view(left_view);
	if (!left_mat_type) {
		spdlog::error(
			"unsupported ZED direct left view {} for SHM binding",
			static_cast<int>(left_view));
		return std::nullopt;
	}

	const auto left_channels = channels_from_mat_type(*left_mat_type);
	auto left_depth = depth_from_mat_type(*left_mat_type);
	if (left_channels == 0 || !left_depth) {
		spdlog::error(
			"unsupported ZED MAT_TYPE {} for direct SHM binding",
			static_cast<int>(*left_mat_type));
		return std::nullopt;
	}
	if (left_channels != info_out.channels || *left_depth != info_out.depth ||
		guess_pixel_format_for_zed(*left_mat_type) != info_out.pixel_format) {
		spdlog::error(
			"direct packed layout mismatch: view {} resolves to type {} but metadata describes channels={} depth={} pixelFormat={}",
			static_cast<int>(left_view),
			static_cast<int>(*left_mat_type),
			info_out.channels,
			app::to_str(info_out.depth),
			app::to_str(info_out.pixel_format));
		return std::nullopt;
	}

	const auto width = info_out.width;
	const auto height = info_out.height;
	const auto left_row_bytes = static_cast<size_t>(width) *
		static_cast<size_t>(left_channels) * static_cast<size_t>(size_of(*left_depth));
	if (left_row_bytes == 0 || width == 0 || height == 0) {
		spdlog::error("invalid left frame geometry/type for direct packing");
		return std::nullopt;
	}

	DirectOutputLayout layout{};
	layout.width = width;
	layout.height = height;
	layout.left_mat_type = *left_mat_type;
	layout.left_row_bytes = left_row_bytes;
	layout.left_offset = 0;
	layout.left_size = left_row_bytes * static_cast<size_t>(height);
	if (layout.left_size == 0) {
		spdlog::error("left frame compact size is zero");
		return std::nullopt;
	}

	if (!depth_enabled || !depth_requested) {
		return layout;
	}

	layout.aux_row_bytes = static_cast<size_t>(width) * sizeof(float);
	layout.depth_offset = layout.left_size;
	layout.depth_size = layout.aux_row_bytes * static_cast<size_t>(height);
	if (layout.depth_size == 0) {
		spdlog::error("depth plane compact size is zero while depth is enabled");
		return std::nullopt;
	}

	if (options.zed_config.publish_confidence) {
		layout.confidence_offset = layout.depth_offset + layout.depth_size;
		layout.confidence_size = layout.depth_size;
	}

	return layout;
}

bool ZedBackendImpl::bind_direct_output_views_locked(
	std::span<uint8_t> payload,
	const frame_info_t &info_out,
	const bool depth_requested) {
	const auto layout = compute_direct_output_layout_locked(info_out, depth_requested);
	if (!layout) {
		clear_direct_output_bindings_locked();
		return false;
	}

	const auto required_size =
  		layout->left_size + layout->depth_size + layout->confidence_size;
	if (required_size == 0 || payload.size() < required_size || payload.data() == nullptr) {
		spdlog::error(
			"direct output buffer too small: capacity={} required={}",
			payload.size(),
			required_size);
		clear_direct_output_bindings_locked();
		return false;
	}

	const auto same_layout =
		direct_output_binding.layout.width == layout->width &&
		direct_output_binding.layout.height == layout->height &&
		direct_output_binding.layout.left_mat_type == layout->left_mat_type &&
		direct_output_binding.layout.left_row_bytes == layout->left_row_bytes &&
		direct_output_binding.layout.aux_row_bytes == layout->aux_row_bytes &&
		direct_output_binding.layout.left_offset == layout->left_offset &&
		direct_output_binding.layout.left_size == layout->left_size &&
		direct_output_binding.layout.depth_offset == layout->depth_offset &&
		direct_output_binding.layout.depth_size == layout->depth_size &&
		direct_output_binding.layout.confidence_offset == layout->confidence_offset &&
		direct_output_binding.layout.confidence_size == layout->confidence_size;
	const bool same_buffer =
		direct_output_binding.buffer_ptr == payload.data() &&
		direct_output_binding.buffer_size == payload.size();
	if (same_layout && same_buffer) {
		return true;
	}

	auto *buffer = reinterpret_cast<sl::uchar1 *>(payload.data());
	const auto resolution = sl::Resolution(layout->width, layout->height);
	direct_output_binding.left_view = sl::Mat(
		resolution,
		layout->left_mat_type,
		buffer + layout->left_offset,
		layout->left_row_bytes,
		sl::MEM::CPU);
	if (layout->depth_size > 0) {
		direct_output_binding.depth_view = sl::Mat(
			resolution,
			sl::MAT_TYPE::F32_C1,
			buffer + layout->depth_offset,
			layout->aux_row_bytes,
			sl::MEM::CPU);
	} else {
		direct_output_binding.depth_view = sl::Mat{};
	}
	if (layout->confidence_size > 0) {
		direct_output_binding.confidence_view = sl::Mat(
			resolution,
			sl::MAT_TYPE::F32_C1,
			buffer + layout->confidence_offset,
			layout->aux_row_bytes,
			sl::MEM::CPU);
	} else {
		direct_output_binding.confidence_view = sl::Mat{};
	}

	direct_output_binding.buffer_ptr = payload.data();
	direct_output_binding.buffer_size = payload.size();
	direct_output_binding.layout = *layout;
	return true;
}

bool ZedBackendImpl::retrieve_direct_measure_plane_locked(
	sl::Mat &target,
	const sl::MEASURE measure,
	const std::span<uint8_t> plane_payload,
	const char *label) {
	if (plane_payload.empty()) {
		return false;
	}

	const auto result = camera.retrieveMeasure(
		target,
		measure,
		sl::MEM::CPU,
		sl::Resolution(
			direct_output_binding.layout.width,
			direct_output_binding.layout.height));
	const auto *target_ptr = target.getPtr<sl::uchar1>(sl::MEM::CPU);
	const bool geometry_valid =
		result == sl::ERROR_CODE::SUCCESS &&
		target.getDataType() == sl::MAT_TYPE::F32_C1 &&
		target.getWidth() == direct_output_binding.layout.width &&
		target.getHeight() == direct_output_binding.layout.height &&
		target_ptr == plane_payload.data() &&
		target.getStepBytes(sl::MEM::CPU) == direct_output_binding.layout.aux_row_bytes;
	if (geometry_valid) {
		return true;
	}

	std::fill(plane_payload.begin(), plane_payload.end(), 0);
	if (result != sl::ERROR_CODE::SUCCESS) {
		spdlog::error(
			"ZED direct retrieveMeasure({}) error: code={}; zeroing plane",
			label,
			static_cast<int>(result));
	} else {
		spdlog::error(
			"ZED direct {} plane shape/binding mismatch; zeroing plane",
			label);
	}
	return false;
}

std::optional<size_t> ZedBackendImpl::pack_frame_locked(
	std::span<uint8_t> payload,
	frame_info_t &info_out,
	const bool direct_output,
	const bool depth_requested) {
	int height = 0;
	std::optional<size_t> left_row_bytes;
	size_t packed_left_size = 0;
	if (direct_output) {
		const auto layout = compute_direct_output_layout_locked(info_out, depth_requested);
		if (!layout) {
			return std::nullopt;
		}
		packed_left_size = layout->left_size;
	} else {
		height = left_frame.getHeight();
		left_row_bytes = expected_row_bytes(left_frame);
		if (!left_row_bytes || height <= 0) {
			spdlog::error("invalid left frame geometry/type for compact packing");
			return std::nullopt;
		}

		packed_left_size = (*left_row_bytes) * static_cast<size_t>(height);
		if (packed_left_size == 0) {
			spdlog::error("left frame compact size is zero");
			return std::nullopt;
		}
	}

	const bool has_depth_payload = depth_enabled && depth_requested;
	if (!has_depth_payload && direct_output) {
		invalidate_direct_depth_tracking_locked();
	}

	size_t packed_depth_size = 0;
	size_t packed_confidence_size = 0;
	size_t depth_row_bytes = 0;
	bool depth_plane_available = false;
	bool confidence_plane_available = false;

	if (direct_output) {
		if (!bind_direct_output_views_locked(payload, info_out, depth_requested)) {
			return std::nullopt;
		}
		packed_depth_size = direct_output_binding.layout.depth_size;
		packed_confidence_size = direct_output_binding.layout.confidence_size;

		// This is one SDK CPU copy straight into SHM-backed views, not camera-memory zero-copy.
		const auto left_result = camera.retrieveImage(
			direct_output_binding.left_view,
			left_view,
			sl::MEM::CPU);
		const auto *left_ptr =
			direct_output_binding.left_view.getPtr<sl::uchar1>(sl::MEM::CPU);
		const bool left_geometry_valid =
			left_result == sl::ERROR_CODE::SUCCESS &&
			direct_output_binding.left_view.getDataType() ==
				direct_output_binding.layout.left_mat_type &&
			direct_output_binding.left_view.getWidth() ==
				direct_output_binding.layout.width &&
			direct_output_binding.left_view.getHeight() ==
				direct_output_binding.layout.height &&
			left_ptr == payload.data() + direct_output_binding.layout.left_offset &&
			direct_output_binding.left_view.getStepBytes(sl::MEM::CPU) ==
				direct_output_binding.layout.left_row_bytes;
		if (!left_geometry_valid) {
			if (left_result != sl::ERROR_CODE::SUCCESS) {
				spdlog::error(
					"ZED direct retrieveImage error: code={}",
					static_cast<int>(left_result));
			} else {
				spdlog::error(
					"ZED direct left plane shape/binding mismatch");
			}
			clear_direct_output_bindings_locked();
			return std::nullopt;
		}

		if (packed_depth_size > 0) {
			auto depth_payload = payload.subspan(
				direct_output_binding.layout.depth_offset,
				direct_output_binding.layout.depth_size);
			if (retrieve_direct_measure_plane_locked(
					direct_output_binding.depth_view,
					sl::MEASURE::DEPTH,
					depth_payload,
					"DEPTH")) {
				last_direct_depth_payload_ptr = depth_payload.data();
				last_direct_depth_payload_size = depth_payload.size();
			} else {
				invalidate_direct_depth_tracking_locked();
			}
		}

		if (packed_confidence_size > 0) {
			auto confidence_payload = payload.subspan(
				direct_output_binding.layout.confidence_offset,
				direct_output_binding.layout.confidence_size);
			if (!retrieve_direct_measure_plane_locked(
					direct_output_binding.confidence_view,
					sl::MEASURE::CONFIDENCE,
					confidence_payload,
					"CONFIDENCE")) {
				packed_confidence_size = 0;
			}
		}

		info_out.buffer_size = static_cast<uint32_t>(
			packed_left_size + packed_depth_size + packed_confidence_size);
		return static_cast<size_t>(info_out.buffer_size);
	}

	if (has_depth_payload) {
		depth_row_bytes =
			static_cast<size_t>(left_frame.getWidth()) * sizeof(float);
		const size_t depth_total_bytes =
			depth_row_bytes * static_cast<size_t>(height);
		if (depth_total_bytes == 0) {
			spdlog::error(
				"depth plane compact size is zero while depth is enabled");
			return std::nullopt;
		}

		packed_depth_size = depth_total_bytes;
		const auto depth_result = camera.retrieveMeasure(
			depth_frame,
			sl::MEASURE::DEPTH,
			sl::MEM::CPU,
			sl::Resolution(left_frame.getWidth(), left_frame.getHeight()));

		const auto depth_geometry_valid =
			depth_frame.getDataType() == sl::MAT_TYPE::F32_C1 &&
			depth_frame.getWidth() == left_frame.getWidth() &&
			depth_frame.getHeight() == left_frame.getHeight();
		depth_plane_available =
			depth_result == sl::ERROR_CODE::SUCCESS && depth_geometry_valid;
		if (!depth_plane_available) {
			if (depth_result != sl::ERROR_CODE::SUCCESS) {
				spdlog::warn(
					"ZED retrieveMeasure(DEPTH) error: code={}; using stable fallback depth bytes",
					static_cast<int>(depth_result));
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
			confidence_plane_available =
				confidence_result == sl::ERROR_CODE::SUCCESS &&
				confidence_geometry_valid;
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

	const auto packed_size =
		packed_left_size + packed_depth_size + packed_confidence_size;
	if (packed_size == 0 ||
		packed_size > std::numeric_limits<uint32_t>::max()) {
		spdlog::error(
			"invalid packed frame size: left={} depth={} confidence={} total={}",
			packed_left_size,
			packed_depth_size,
			packed_confidence_size,
			packed_size);
		return std::nullopt;
	}
	if (payload.size() < packed_size) {
		spdlog::error(
			"packed output buffer too small: capacity={} required={}",
			payload.size(),
			packed_size);
		return std::nullopt;
	}

	if (!copy_compact_plane(
			left_frame,
			*left_row_bytes,
			payload.subspan(0, packed_left_size))) {
		spdlog::error("bad left-plane compaction into packed payload");
		return std::nullopt;
	}

	if (has_depth_payload) {
		auto depth_payload = payload.subspan(packed_left_size, packed_depth_size);
		if (depth_plane_available &&
			copy_compact_plane(depth_frame, depth_row_bytes, depth_payload)) {
			last_good_depth_plane.resize(packed_depth_size);
			std::memcpy(
				last_good_depth_plane.data(),
				depth_payload.data(),
				packed_depth_size);
			invalidate_direct_depth_tracking_locked();
		} else {
			if (depth_plane_available) {
				spdlog::warn(
					"ZED depth plane compaction error; using stable fallback depth bytes");
			}
			if (last_good_depth_plane.size() == packed_depth_size) {
				std::memcpy(
					depth_payload.data(),
					last_good_depth_plane.data(),
					packed_depth_size);
			} else {
				std::fill(depth_payload.begin(), depth_payload.end(), 0);
			}
			invalidate_direct_depth_tracking_locked();
		}
	}
	if (packed_confidence_size > 0) {
		auto confidence_payload = payload.subspan(
			packed_left_size + packed_depth_size,
			packed_confidence_size);
		if (!copy_compact_plane(
				confidence_frame,
				depth_row_bytes,
				confidence_payload)) {
			spdlog::warn(
				"ZED confidence plane compaction error; publishing left/depth only");
			packed_confidence_size = 0;
		}
	}

	info_out.buffer_size = static_cast<uint32_t>(
		packed_left_size + packed_depth_size + packed_confidence_size);
	return static_cast<size_t>(info_out.buffer_size);
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
		info = make_direct_frame_info(camera, left_view);
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

std::optional<frame_info_t> ZedBackendImpl::make_frame_info(const sl::Mat &frame) {
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
		spdlog::error(
			"unsupported ZED MAT_TYPE depth mapping: {}",
			static_cast<int>(mat_type));
		return std::nullopt;
	}

	auto row_bytes = expected_row_bytes(frame);
	if (!row_bytes) {
		spdlog::error(
			"cannot compute compact row bytes for ZED MAT_TYPE: {}",
			static_cast<int>(mat_type));
		return std::nullopt;
	}

	const auto buffer_size = (*row_bytes) * static_cast<size_t>(height);
	if (buffer_size == 0 ||
		buffer_size > std::numeric_limits<uint32_t>::max()) {
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

ZedBackendImpl::PublishedFrame ZedBackendImpl::publish_captured_frame(
	CapturedFrame captured,
	std::vector<uint8_t> payload,
	const uint32_t frame_count,
	const bool log_publish_gap) {
	PublishedFrame published_frame{};
	published_frame.metadata.frame_count = frame_count;
	published_frame.metadata.timestamp_ns = captured.timestamp_ns;
	published_frame.metadata.info = captured.info;
	published_frame.metadata.info.buffer_size =
		static_cast<uint32_t>(payload.size());
	published_frame.payload = std::move(payload);
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
			std::span<uint8_t> output_buffer) mutable -> std::optional<size_t> {
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
		published_frame.metadata);
	if (published_frame.body_tracking) {
		on_body_tracking(*published_frame.body_tracking);
	}
}

void ZedBackendImpl::emit_published_frame_direct(
	DirectPublishedFrame published_frame) {
	on_direct_frame(direct_frame_t{
		.metadata = published_frame.metadata,
		.depth_unit = body_tracking_enabled ? DepthUnit::Meter : DepthUnit::Millimeter,
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
		std::optional<size_t> packed_size;
		{
			std::lock_guard lock(camera_mutex);
			packed_size = pack_frame_locked(
				std::span<uint8_t>(payload.data(), payload.size()),
				published_frame.metadata.info,
				false,
				capture->depth_requested);
		}
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

} // namespace app::backends
