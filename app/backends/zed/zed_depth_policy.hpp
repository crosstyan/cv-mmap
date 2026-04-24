#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace app::backends {

enum class ZedDepthFpsSource {
	Disabled,
	Reported,
	Configured,
	DeterministicFallback,
};

struct ZedDepthCadenceConfig {
	bool depth_enabled{false};
	double reported_source_fps{0.0};
	double configured_source_fps{0.0};
	int depth_max_fps{0};
};

struct ZedDepthCadenceSnapshot {
	bool depth_enabled{false};
	ZedDepthFpsSource fps_source{ZedDepthFpsSource::Disabled};
	double effective_source_fps{0.0};
	uint32_t period_frames{1};
	int configured_depth_max_fps{0};
};

class ZedDepthCadencePolicy {
public:
	ZedDepthCadenceSnapshot configure(ZedDepthCadenceConfig config);
	void reset(bool depth_enabled);

	[[nodiscard]] bool should_request(bool force_depth_request) const;
	void commit(bool depth_requested);

	[[nodiscard]] bool depth_enabled() const { return depth_enabled_; }
	[[nodiscard]] double effective_source_fps() const {
		return effective_source_fps_;
	}
	[[nodiscard]] uint32_t period_frames() const { return period_frames_; }
	[[nodiscard]] uint32_t frames_since_last_request() const {
		return frames_since_last_request_;
	}
	[[nodiscard]] bool force_next_capture() const {
		return force_next_capture_;
	}

private:
	[[nodiscard]] ZedDepthCadenceSnapshot snapshot() const;

	bool depth_enabled_{false};
	int depth_max_fps_{0};
	ZedDepthFpsSource fps_source_{ZedDepthFpsSource::Disabled};
	double effective_source_fps_{0.0};
	uint32_t period_frames_{1};
	uint32_t frames_since_last_request_{0};
	bool force_next_capture_{true};
};

class ZedDepthFallbackPolicy {
public:
	void reset();
	void invalidate_direct_tracking();

	void store_good_plane(std::span<const uint8_t> plane);
	[[nodiscard]] bool copy_or_zero_fill(std::span<uint8_t> plane) const;

	void track_direct_plane(std::span<const uint8_t> plane);
	[[nodiscard]] bool snapshot_tracked_direct_plane(
		std::span<const uint8_t> containing_buffer);

	[[nodiscard]] size_t last_good_plane_size() const {
		return last_good_depth_plane_.size();
	}
	[[nodiscard]] bool has_direct_tracking() const {
		return last_direct_depth_payload_ptr_ != nullptr &&
			last_direct_depth_payload_size_ > 0;
	}

private:
	std::vector<uint8_t> last_good_depth_plane_{};
	const uint8_t *last_direct_depth_payload_ptr_{nullptr};
	size_t last_direct_depth_payload_size_{0};
};

} // namespace app::backends
