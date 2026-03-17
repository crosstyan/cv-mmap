#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "app_backends_dummy.hpp"
#include "app_config.hpp"

namespace {

using app::BackendType;
using app::DummyConfig;
using app::FiniteStreamEndingBehavior;
using app::VideoConfig;
using app::backends::DummyBackend;

struct CapturedFrame {
	uint32_t frame_count{0};
	uint64_t timestamp_ns{0};
};

struct FrameCollector {
	mutable std::mutex mutex;
	std::condition_variable cv;
	std::vector<CapturedFrame> frames;

	void push(const uint32_t frame_count, const uint64_t timestamp_ns) {
		{
			std::lock_guard lock(mutex);
			frames.push_back(CapturedFrame{
				.frame_count = frame_count,
				.timestamp_ns = timestamp_ns,
			});
		}
		cv.notify_all();
	}

	bool wait_for_count(
		const size_t expected_count,
		const std::chrono::milliseconds timeout) {
		std::unique_lock lock(mutex);
		return cv.wait_for(lock, timeout, [&] {
			return frames.size() >= expected_count;
		});
	}

	[[nodiscard]]
	CapturedFrame at(const size_t index) const {
		std::lock_guard lock(mutex);
		return frames.at(index);
	}
};

[[nodiscard]]
std::array<uint8_t, 3> expected_dummy_pattern_pixel(
	const DummyConfig &config,
	uint32_t frame_count,
	int x,
	int y) {
	return {
		static_cast<uint8_t>((x + frame_count * 3u) % 256u),
		static_cast<uint8_t>((y * 2u + frame_count * 5u) % 256u),
		static_cast<uint8_t>(((x / 2u) + (y / 3u) + frame_count * 7u) % 256u),
	};
}

int test_overlay_modifies_top_left_pixels() {
	DummyConfig dummy_cfg{};
	dummy_cfg.width = 320;
	dummy_cfg.height = 120;
	dummy_cfg.fps = 30;
	dummy_cfg.frames = 2;

	VideoConfig video_cfg{};
	video_cfg.backend = BackendType::Dummy;
	video_cfg.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Stop;

	DummyBackend backend(dummy_cfg, video_cfg);
	std::vector<uint8_t> first_frame;
	bool got_frame = false;
	backend.SetOnFrame(
		[&](std::span<uint8_t> frame, const auto &metadata) {
			if (!got_frame) {
				first_frame.assign(frame.begin(), frame.end());
				got_frame = true;
			}
			(void)metadata;
		});
	backend.Init();
	backend.Shutdown();

	if (!got_frame) {
		return 1;
	}

	const auto expected = expected_dummy_pattern_pixel(dummy_cfg, 0, 1, 1);
	const auto pixel_index = static_cast<size_t>(((1 * dummy_cfg.width) + 1) * 3);
	if (first_frame[pixel_index + 0] == expected[0] &&
		first_frame[pixel_index + 1] == expected[1] &&
		first_frame[pixel_index + 2] == expected[2]) {
		return 2;
	}

	return 0;
}

int test_reset_rewinds_to_first_finite_frame() {
	DummyConfig dummy_cfg{};
	dummy_cfg.fps = 10;
	dummy_cfg.frames = 4;

	VideoConfig video_cfg{};
	video_cfg.backend = BackendType::Dummy;
	video_cfg.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Loop;

	DummyBackend backend(dummy_cfg, video_cfg);
	FrameCollector collector;
	backend.SetOnFrame(
		[&](std::span<uint8_t>, const auto &metadata) {
			collector.push(metadata.frame_count, metadata.timestamp_ns);
		});
	backend.Init();
	if (!collector.wait_for_count(1, std::chrono::milliseconds(100))) {
		backend.Shutdown();
		return 1;
	}
	if (backend.ResetFrameCount() != app::backends::ERR_OK) {
		backend.Shutdown();
		return 2;
	}
	if (!collector.wait_for_count(2, std::chrono::milliseconds(250))) {
		backend.Shutdown();
		return 3;
	}
	backend.Shutdown();

	const auto rewound = collector.at(1);
	if (rewound.frame_count != 0) {
		return 4;
	}
	if (rewound.timestamp_ns != 0) {
		return 5;
	}
	return 0;
}

int test_seek_emits_landed_finite_frame() {
	DummyConfig dummy_cfg{};
	dummy_cfg.fps = 10;
	dummy_cfg.frames = 5;

	VideoConfig video_cfg{};
	video_cfg.backend = BackendType::Dummy;
	video_cfg.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Stop;

	DummyBackend backend(dummy_cfg, video_cfg);
	FrameCollector collector;
	backend.SetOnFrame(
		[&](std::span<uint8_t>, const auto &metadata) {
			collector.push(metadata.frame_count, metadata.timestamp_ns);
		});
	backend.Init();
	if (!collector.wait_for_count(1, std::chrono::milliseconds(100))) {
		backend.Shutdown();
		return 1;
	}

	const auto target_timestamp_ns = 200000000ull;
	const auto seek_result = backend.SeekTimestampNs(target_timestamp_ns);
	if (!seek_result) {
		backend.Shutdown();
		return 2;
	}
	if (!collector.wait_for_count(2, std::chrono::milliseconds(250))) {
		backend.Shutdown();
		return 3;
	}
	backend.Shutdown();

	const auto landed = collector.at(1);
	if (landed.frame_count != 0) {
		return 4;
	}
	if (landed.timestamp_ns != target_timestamp_ns) {
		return 5;
	}
	return 0;
}

int test_loop_silent_wraps_to_first_finite_frame() {
	DummyConfig dummy_cfg{};
	dummy_cfg.fps = 20;
	dummy_cfg.frames = 2;

	VideoConfig video_cfg{};
	video_cfg.backend = BackendType::Dummy;
	video_cfg.finite_stream_ending_behavior = FiniteStreamEndingBehavior::LoopSilent;

	DummyBackend backend(dummy_cfg, video_cfg);
	FrameCollector collector;
	backend.SetOnFrame(
		[&](std::span<uint8_t>, const auto &metadata) {
			collector.push(metadata.frame_count, metadata.timestamp_ns);
		});
	backend.Init();
	if (!collector.wait_for_count(3, std::chrono::milliseconds(250))) {
		backend.Shutdown();
		return 1;
	}
	backend.Shutdown();

	const auto wrapped = collector.at(2);
	if (wrapped.frame_count != 0) {
		return 2;
	}
	if (wrapped.timestamp_ns != 0) {
		return 3;
	}
	return 0;
}

int test_invalid_override_font_path_fails_startup() {
	DummyConfig dummy_cfg{};
	dummy_cfg.timestamp_overlay_font_path = "/definitely/not/a/font.ttf";

	VideoConfig video_cfg{};
	video_cfg.backend = BackendType::Dummy;
	video_cfg.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Stop;

	DummyBackend backend(dummy_cfg, video_cfg);
	try {
		backend.Init();
		backend.Shutdown();
		return 1;
	} catch (const std::exception &) {
		return 0;
	}
}

int test_finite_source_info_stays_seekable() {
	DummyConfig dummy_cfg{};
	dummy_cfg.frames = 5;

	VideoConfig video_cfg{};
	video_cfg.backend = BackendType::Dummy;
	video_cfg.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Loop;

	DummyBackend backend(dummy_cfg, video_cfg);
	backend.Init();
	const auto source_info = backend.GetSourceInfo();
	backend.Shutdown();

	if (source_info.source_kind != cvmmap::SourceKind::Finite) {
		return 1;
	}
	if (source_info.timestamp_domain != cvmmap::TimestampDomain::MediaTimeNs) {
		return 2;
	}
	if ((source_info.flags & cvmmap::SOURCE_INFO_FLAG_CAN_SEEK) == 0) {
		return 3;
	}
	if ((source_info.flags & cvmmap::SOURCE_INFO_FLAG_AUTO_LOOP) == 0) {
		return 4;
	}
	if ((source_info.flags & cvmmap::SOURCE_INFO_FLAG_LOOP_EMITS_RESET) == 0) {
		return 5;
	}

	return 0;
}

} // namespace

int main() {
	if (const auto rc = test_overlay_modifies_top_left_pixels(); rc != 0) {
		return 10 + rc;
	}
	if (const auto rc = test_reset_rewinds_to_first_finite_frame(); rc != 0) {
		return 20 + rc;
	}
	if (const auto rc = test_seek_emits_landed_finite_frame(); rc != 0) {
		return 30 + rc;
	}
	if (const auto rc = test_loop_silent_wraps_to_first_finite_frame(); rc != 0) {
		return 40 + rc;
	}
	if (const auto rc = test_invalid_override_font_path_fails_startup(); rc != 0) {
		return 50 + rc;
	}
	if (const auto rc = test_finite_source_info_stays_seekable(); rc != 0) {
		return 60 + rc;
	}
	return 0;
}
