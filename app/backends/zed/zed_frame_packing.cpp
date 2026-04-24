#include "zed_backend_internal.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

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

} // namespace

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

std::optional<frame_info_t>
ZedBackendImpl::make_direct_frame_info_locked() const {
	const auto resolution =
		camera.getCameraInformation().camera_configuration.resolution;
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
		static_cast<size_t>(left_channels) *
		static_cast<size_t>(size_of(*left_depth));
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
	const auto layout =
		compute_direct_output_layout_locked(info_out, depth_requested);
	if (!layout) {
		clear_direct_output_bindings_locked();
		return false;
	}

	const auto required_size =
		layout->left_size + layout->depth_size + layout->confidence_size;
	if (required_size == 0 || payload.size() < required_size ||
		payload.data() == nullptr) {
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
		direct_output_binding.layout.confidence_offset ==
			layout->confidence_offset &&
		direct_output_binding.layout.confidence_size ==
			layout->confidence_size;
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
		target.getStepBytes(sl::MEM::CPU) ==
			direct_output_binding.layout.aux_row_bytes;
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

std::optional<direct_frame_fill_result_t> ZedBackendImpl::pack_frame_locked(
	std::span<uint8_t> payload,
	frame_info_t &info_out,
	const bool direct_output,
	const bool depth_requested) {
	int height = 0;
	std::optional<size_t> left_row_bytes;
	size_t packed_left_size = 0;
	if (direct_output) {
		const auto layout =
			compute_direct_output_layout_locked(info_out, depth_requested);
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
				depth_fallback.track_direct_plane(depth_payload);
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

		const auto packed_size =
			packed_left_size + packed_depth_size + packed_confidence_size;
		info_out.buffer_size = static_cast<uint32_t>(packed_size);
		frame_metadata_t metadata_for_layout{};
		metadata_for_layout.info = info_out;
		const auto depth_unit =
			body_tracking_enabled ? DepthUnit::Meter : DepthUnit::Millimeter;
		auto layout = packed_confidence_size > 0
			? make_left_depth_confidence_payload_layout(
				  metadata_for_layout,
				  packed_left_size,
				  packed_depth_size,
				  packed_confidence_size,
				  depth_unit)
			: packed_depth_size > 0
				? make_left_depth_payload_layout(
					  metadata_for_layout,
					  packed_left_size,
					  packed_depth_size,
					  depth_unit)
				: make_left_only_payload_layout(
					  metadata_for_layout,
					  packed_left_size);
		return direct_frame_fill_result_t{
			.payload_size_bytes = packed_size,
			.layout = std::move(layout),
		};
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
			depth_fallback.store_good_plane(depth_payload);
			invalidate_direct_depth_tracking_locked();
		} else {
			if (depth_plane_available) {
				spdlog::warn(
					"ZED depth plane compaction error; using stable fallback depth bytes");
			}
			(void)depth_fallback.copy_or_zero_fill(depth_payload);
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
	frame_metadata_t metadata_for_layout{};
	metadata_for_layout.info = info_out;
	const auto depth_unit =
		body_tracking_enabled ? DepthUnit::Meter : DepthUnit::Millimeter;
	auto layout = packed_confidence_size > 0
		? make_left_depth_confidence_payload_layout(
			  metadata_for_layout,
			  packed_left_size,
			  packed_depth_size,
			  packed_confidence_size,
			  depth_unit)
		: packed_depth_size > 0
			? make_left_depth_payload_layout(
				  metadata_for_layout,
				  packed_left_size,
				  packed_depth_size,
				  depth_unit)
			: make_left_only_payload_layout(metadata_for_layout, packed_left_size);
	return direct_frame_fill_result_t{
		.payload_size_bytes = static_cast<size_t>(info_out.buffer_size),
		.layout = std::move(layout),
	};
}

std::optional<frame_info_t> ZedBackendImpl::make_frame_info(
	const sl::Mat &frame) {
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

} // namespace app::backends
