#include "zed/zed_depth_policy.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using app::backends::ZedDepthCadenceConfig;
using app::backends::ZedDepthCadencePolicy;
using app::backends::ZedDepthFallbackPolicy;
using app::backends::ZedDepthFpsSource;

bool expect(bool condition) {
	return condition;
}

bool test_disabled_cadence_never_requests_depth() {
	ZedDepthCadencePolicy cadence{};
	const auto snapshot = cadence.configure(ZedDepthCadenceConfig{
		.depth_enabled = false,
		.reported_source_fps = 60,
		.configured_source_fps = 30,
		.depth_max_fps = 15,
	});

	if (!expect(!snapshot.depth_enabled)) {
		return false;
	}
	if (!expect(snapshot.fps_source == ZedDepthFpsSource::Disabled)) {
		return false;
	}
	if (!expect(snapshot.effective_source_fps == 0.0)) {
		return false;
	}
	if (!expect(!cadence.should_request(true))) {
		return false;
	}
	cadence.commit(true);
	return expect(cadence.frames_since_last_request() == 0);
}

bool test_depth_max_fps_cadence_counts_skipped_frames() {
	ZedDepthCadencePolicy cadence{};
	const auto snapshot = cadence.configure(ZedDepthCadenceConfig{
		.depth_enabled = true,
		.reported_source_fps = 60,
		.configured_source_fps = 0,
		.depth_max_fps = 15,
	});

	if (!expect(snapshot.fps_source == ZedDepthFpsSource::Reported)) {
		return false;
	}
	if (!expect(snapshot.effective_source_fps == 60.0)) {
		return false;
	}
	if (!expect(snapshot.period_frames == 4)) {
		return false;
	}
	if (!expect(cadence.should_request(false))) {
		return false;
	}
	cadence.commit(true);
	if (!expect(!cadence.should_request(false))) {
		return false;
	}
	cadence.commit(false);
	if (!expect(!cadence.should_request(false))) {
		return false;
	}
	cadence.commit(false);
	if (!expect(!cadence.should_request(false))) {
		return false;
	}
	cadence.commit(false);
	return expect(cadence.should_request(false));
}

bool test_cadence_uses_configured_and_deterministic_fps_fallbacks() {
	ZedDepthCadencePolicy cadence{};
	auto snapshot = cadence.configure(ZedDepthCadenceConfig{
		.depth_enabled = true,
		.reported_source_fps = 0,
		.configured_source_fps = 30,
		.depth_max_fps = 12,
	});
	if (!expect(snapshot.fps_source == ZedDepthFpsSource::Configured)) {
		return false;
	}
	if (!expect(snapshot.effective_source_fps == 30.0)) {
		return false;
	}
	if (!expect(snapshot.period_frames == 3)) {
		return false;
	}

	snapshot = cadence.configure(ZedDepthCadenceConfig{
		.depth_enabled = true,
		.reported_source_fps = -1,
		.configured_source_fps = 0,
		.depth_max_fps = 2,
	});
	if (!expect(snapshot.fps_source == ZedDepthFpsSource::DeterministicFallback)) {
		return false;
	}
	if (!expect(snapshot.effective_source_fps == 1.0)) {
		return false;
	}
	return expect(snapshot.period_frames == 1);
}

bool test_unlimited_depth_cadence_requests_every_frame() {
	ZedDepthCadencePolicy cadence{};
	cadence.configure(ZedDepthCadenceConfig{
		.depth_enabled = true,
		.reported_source_fps = 60,
		.configured_source_fps = 0,
		.depth_max_fps = 0,
	});
	if (!expect(cadence.should_request(false))) {
		return false;
	}
	cadence.commit(true);
	if (!expect(cadence.should_request(false))) {
		return false;
	}
	cadence.commit(false);
	return expect(cadence.should_request(false));
}

bool test_fallback_cache_copies_or_zero_fills_by_exact_size() {
	ZedDepthFallbackPolicy fallback{};
	const std::array<uint8_t, 4> good{{4, 3, 2, 1}};
	fallback.store_good_plane(good);

	std::array<uint8_t, 4> reused{{0, 0, 0, 0}};
	if (!expect(fallback.copy_or_zero_fill(reused))) {
		return false;
	}
	if (!expect(reused == good)) {
		return false;
	}

	std::array<uint8_t, 5> wrong_size{{9, 9, 9, 9, 9}};
	if (!expect(!fallback.copy_or_zero_fill(wrong_size))) {
		return false;
	}
	if (!expect(wrong_size == std::array<uint8_t, 5>{{0, 0, 0, 0, 0}})) {
		return false;
	}

	fallback.reset();
	std::array<uint8_t, 4> after_reset{{9, 9, 9, 9}};
	if (!expect(!fallback.copy_or_zero_fill(after_reset))) {
		return false;
	}
	return expect(after_reset == std::array<uint8_t, 4>{{0, 0, 0, 0}});
}

bool test_direct_snapshot_only_accepts_pointer_inside_reset_buffer() {
	ZedDepthFallbackPolicy fallback{};
	std::vector<uint8_t> buffer{0, 1, 2, 3, 4, 5, 6, 7};
	fallback.track_direct_plane(std::span<const uint8_t>(buffer.data() + 2, 3));
	if (!expect(fallback.has_direct_tracking())) {
		return false;
	}
	if (!expect(fallback.snapshot_tracked_direct_plane(buffer))) {
		return false;
	}
	if (!expect(!fallback.has_direct_tracking())) {
		return false;
	}

	std::array<uint8_t, 3> copied{{0, 0, 0}};
	if (!expect(fallback.copy_or_zero_fill(copied))) {
		return false;
	}
	if (!expect(copied == std::array<uint8_t, 3>{{2, 3, 4}})) {
		return false;
	}

	std::vector<uint8_t> other{8, 9, 10};
	fallback.track_direct_plane(other);
	if (!expect(!fallback.snapshot_tracked_direct_plane(buffer))) {
		return false;
	}
	if (!expect(!fallback.has_direct_tracking())) {
		return false;
	}
	copied = {0, 0, 0};
	if (!expect(fallback.copy_or_zero_fill(copied))) {
		return false;
	}
	return expect(copied == std::array<uint8_t, 3>{{2, 3, 4}});
}

} // namespace

int main() {
	if (!test_disabled_cadence_never_requests_depth()) {
		return 1;
	}
	if (!test_depth_max_fps_cadence_counts_skipped_frames()) {
		return 2;
	}
	if (!test_cadence_uses_configured_and_deterministic_fps_fallbacks()) {
		return 3;
	}
	if (!test_unlimited_depth_cadence_requests_every_frame()) {
		return 4;
	}
	if (!test_fallback_cache_copies_or_zero_fills_by_exact_size()) {
		return 5;
	}
	if (!test_direct_snapshot_only_accepts_pointer_inside_reset_buffer()) {
		return 6;
	}
	return 0;
}
