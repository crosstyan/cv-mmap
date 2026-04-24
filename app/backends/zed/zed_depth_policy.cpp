#include "zed_depth_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace app::backends {

ZedDepthCadenceSnapshot ZedDepthCadencePolicy::configure(
	ZedDepthCadenceConfig config) {
	depth_enabled_ = config.depth_enabled;
	depth_max_fps_ = std::max(0, config.depth_max_fps);
	fps_source_ = ZedDepthFpsSource::Disabled;
	effective_source_fps_ = 0.0;
	period_frames_ = 1;
	frames_since_last_request_ = 0;
	force_next_capture_ = true;

	if (!depth_enabled_) {
		return snapshot();
	}

	if (config.reported_source_fps > 0) {
		fps_source_ = ZedDepthFpsSource::Reported;
		effective_source_fps_ = config.reported_source_fps;
	} else if (config.configured_source_fps > 0) {
		fps_source_ = ZedDepthFpsSource::Configured;
		effective_source_fps_ = config.configured_source_fps;
	} else {
		fps_source_ = ZedDepthFpsSource::DeterministicFallback;
		effective_source_fps_ = 1.0;
	}

	if (depth_max_fps_ > 0) {
		period_frames_ = std::max<uint32_t>(
			1u,
			static_cast<uint32_t>(
				std::ceil(effective_source_fps_ /
						  static_cast<double>(depth_max_fps_))));
	}

	return snapshot();
}

void ZedDepthCadencePolicy::reset(const bool depth_enabled) {
	depth_enabled_ = depth_enabled;
	frames_since_last_request_ = 0;
	force_next_capture_ = true;
}

bool ZedDepthCadencePolicy::should_request(
	const bool force_depth_request) const {
	if (!depth_enabled_) {
		return false;
	}
	if (force_depth_request || force_next_capture_) {
		return true;
	}
	if (depth_max_fps_ <= 0) {
		return true;
	}
	return frames_since_last_request_ + 1 >= period_frames_;
}

void ZedDepthCadencePolicy::commit(const bool depth_requested) {
	if (!depth_enabled_) {
		return;
	}
	force_next_capture_ = false;
	if (depth_requested) {
		frames_since_last_request_ = 0;
		return;
	}
	if (frames_since_last_request_ < std::numeric_limits<uint32_t>::max()) {
		frames_since_last_request_ += 1;
	}
}

ZedDepthCadenceSnapshot ZedDepthCadencePolicy::snapshot() const {
	return ZedDepthCadenceSnapshot{
		.depth_enabled = depth_enabled_,
		.fps_source = fps_source_,
		.effective_source_fps = effective_source_fps_,
		.period_frames = period_frames_,
		.configured_depth_max_fps = depth_max_fps_,
	};
}

void ZedDepthFallbackPolicy::reset() {
	last_good_depth_plane_.clear();
	invalidate_direct_tracking();
}

void ZedDepthFallbackPolicy::invalidate_direct_tracking() {
	last_direct_depth_payload_ptr_ = nullptr;
	last_direct_depth_payload_size_ = 0;
}

void ZedDepthFallbackPolicy::store_good_plane(
	std::span<const uint8_t> plane) {
	last_good_depth_plane_.resize(plane.size());
	if (!plane.empty()) {
		std::memcpy(last_good_depth_plane_.data(), plane.data(), plane.size());
	}
}

bool ZedDepthFallbackPolicy::copy_or_zero_fill(
	std::span<uint8_t> plane) const {
	if (last_good_depth_plane_.size() == plane.size()) {
		if (!plane.empty()) {
			std::memcpy(plane.data(), last_good_depth_plane_.data(), plane.size());
		}
		return true;
	}
	std::fill(plane.begin(), plane.end(), 0);
	return false;
}

void ZedDepthFallbackPolicy::track_direct_plane(
	std::span<const uint8_t> plane) {
	last_direct_depth_payload_ptr_ = plane.data();
	last_direct_depth_payload_size_ = plane.size();
	if (plane.empty()) {
		invalidate_direct_tracking();
	}
}

bool ZedDepthFallbackPolicy::snapshot_tracked_direct_plane(
	std::span<const uint8_t> containing_buffer) {
	if (!has_direct_tracking() || containing_buffer.data() == nullptr ||
		containing_buffer.empty()) {
		invalidate_direct_tracking();
		return false;
	}

	const auto buffer_begin =
		reinterpret_cast<std::uintptr_t>(containing_buffer.data());
	const auto buffer_size = containing_buffer.size();
	if (buffer_size >
		std::numeric_limits<std::uintptr_t>::max() - buffer_begin) {
		invalidate_direct_tracking();
		return false;
	}
	const auto buffer_end = buffer_begin + buffer_size;
	const auto depth_begin =
		reinterpret_cast<std::uintptr_t>(last_direct_depth_payload_ptr_);
	if (depth_begin < buffer_begin || depth_begin > buffer_end ||
		last_direct_depth_payload_size_ > buffer_end - depth_begin) {
		invalidate_direct_tracking();
		return false;
	}

	last_good_depth_plane_.resize(last_direct_depth_payload_size_);
	std::memcpy(
		last_good_depth_plane_.data(),
		last_direct_depth_payload_ptr_,
		last_direct_depth_payload_size_);
	invalidate_direct_tracking();
	return true;
}

} // namespace app::backends
