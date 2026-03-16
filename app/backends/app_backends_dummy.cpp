#include <algorithm>
#include <chrono>
#include <cstdint>
#include <errno.h>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "app_backends_dummy.hpp"
#include "app_config.hpp"
#include "app_backends_facade.hpp"
#include "app_enum_models.hpp"

namespace app::backends {

namespace {

using clock_t = std::chrono::steady_clock;

uint64_t now_ns() {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

std::chrono::milliseconds frame_interval_for(const app::DummyConfig &config) {
	return std::chrono::milliseconds(std::max(1, 1000 / std::max(1, config.fps)));
}

uint64_t frame_interval_ns_for(const app::DummyConfig &config) {
	const auto fps = std::max(1, config.fps);
	return 1000000000ull / static_cast<uint64_t>(fps);
}

void render_dummy_pattern(
	std::span<uint8_t> frame_buffer,
	const app::DummyConfig &config,
	const uint32_t frame_count) {
	const auto width  = static_cast<size_t>(config.width);
	const auto height = static_cast<size_t>(config.height);

	for (size_t y = 0; y < height; ++y) {
		for (size_t x = 0; x < width; ++x) {
			const auto pixel_index = (y * width + x) * 3;
			frame_buffer[pixel_index + 0] =
				static_cast<uint8_t>((x + frame_count * 3u) % 256u);
			frame_buffer[pixel_index + 1] =
				static_cast<uint8_t>((y * 2u + frame_count * 5u) % 256u);
			frame_buffer[pixel_index + 2] =
				static_cast<uint8_t>(((x / 2u) + (y / 3u) + frame_count * 7u) % 256u);
		}
	}
}

} // namespace

struct DummyBackendOptions {
	app::DummyConfig dummy_config;
	app::VideoConfig video_config;
};

struct DummyBackendImpl {
	DummyBackendOptions options;
	std::jthread worker_thread;
	on_metadata_fn_t _on_metadata{nullptr};
	on_frame_fn_t _on_frame{nullptr};
	on_error_fn_t _on_error{nullptr};
	frame_metadata_t metadata{};
	std::vector<uint8_t> frame_buffer;
	std::mutex state_mutex;
	uint32_t emitted_frames{0};
	uint32_t source_frame_index{0};

	DummyBackendImpl() = default;
	explicit DummyBackendImpl(DummyBackendOptions opts) : options(std::move(opts)) {}

	void on_metadata(const frame_metadata_t &metadata_) {
		if (_on_metadata) {
			_on_metadata(metadata_);
		}
	}

	void on_frame(std::span<uint8_t> frame_buffer_, const frame_metadata_t &metadata_) {
		if (_on_frame) {
			_on_frame(frame_buffer_, metadata_);
		}
	}

	void on_error(error_t error_code, std::string_view message) {
		if (_on_error) {
			_on_error(error_code, message);
		}
	}

	[[nodiscard]]
	bool is_finite_source() const {
		return options.dummy_config.frames > 0;
	}

	[[nodiscard]]
	uint64_t timestamp_for_frame(uint32_t frame_count) const {
		if (!is_finite_source()) {
			return now_ns();
		}
		return static_cast<uint64_t>(frame_count) *
			   frame_interval_ns_for(options.dummy_config);
	}

	[[nodiscard]]
	source_info_t GetSourceInfo() {
		std::lock_guard lock(state_mutex);

		source_info_t info{};
		info.source_kind =
			is_finite_source() ? cvmmap::SourceKind::Finite
							   : cvmmap::SourceKind::Live;
		info.timestamp_domain =
			is_finite_source() ? cvmmap::TimestampDomain::MediaTimeNs
							   : cvmmap::TimestampDomain::UnixEpochNs;
		if (is_finite_source()) {
			info.timeline_start_ns = 0;
			info.timeline_end_ns =
				static_cast<uint64_t>(std::max<uint32_t>(
					options.dummy_config.frames - 1, 0u)) *
				frame_interval_ns_for(options.dummy_config);
			info.duration_ns =
				static_cast<uint64_t>(options.dummy_config.frames) *
				frame_interval_ns_for(options.dummy_config);
			if (!options.video_config.use_finite_as_infinite_stream) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_SEEK;
			}
			if (options.video_config.finite_stream_ending_behavior ==
				app::FiniteStreamEndingBehavior::Loop ||
				options.video_config.use_finite_as_infinite_stream) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_AUTO_LOOP;
			}
		}
		info.current_timestamp_ns = metadata.timestamp_ns;
		info.current_frame_count = metadata.frame_count;
		return info;
	}

	std::expected<seek_result_t, error_t> SeekTimestampNs(
		uint64_t timestamp_ns) {
		if (!is_finite_source() ||
			options.video_config.use_finite_as_infinite_stream) {
			return std::unexpected(-EOPNOTSUPP);
		}

		const auto interval_ns = frame_interval_ns_for(options.dummy_config);
		const auto max_timestamp_ns =
			static_cast<uint64_t>(std::max<uint32_t>(
				options.dummy_config.frames - 1, 0u)) *
			interval_ns;
		if (timestamp_ns > max_timestamp_ns) {
			return std::unexpected(-ERANGE);
		}

		const auto frame_index = static_cast<uint32_t>(timestamp_ns / interval_ns);

		std::lock_guard lock(state_mutex);
		metadata.frame_count = 0;
		metadata.timestamp_ns = frame_index * interval_ns;
		emitted_frames = frame_index + 1;
		source_frame_index = frame_index;
		render_dummy_pattern(frame_buffer, options.dummy_config, source_frame_index);
		return seek_result_t{
			.requested_timestamp_ns = timestamp_ns,
			.landed_timestamp_ns = metadata.timestamp_ns,
			.landed_frame_count = metadata.frame_count,
			.exact_match = (timestamp_ns % interval_ns) == 0,
		};
	}

	void Init() {
		if (options.dummy_config.startup_delay_ms > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(options.dummy_config.startup_delay_ms));
		}

		metadata.ensure_magic();
		metadata.frame_count  = 0;
		metadata.timestamp_ns = timestamp_for_frame(metadata.frame_count);
		metadata.info         = frame_info_t{
			.width        = static_cast<uint16_t>(options.dummy_config.width),
			.height       = static_cast<uint16_t>(options.dummy_config.height),
			.channels     = 3,
			.depth        = Depth::U8,
			.pixel_format = PixelFormat::BGR,
			.buffer_size  = static_cast<uint32_t>(
				static_cast<uint64_t>(options.dummy_config.width) *
				static_cast<uint64_t>(options.dummy_config.height) * 3ull),
		};

		frame_buffer.resize(metadata.info.buffer_size);
		render_dummy_pattern(frame_buffer, options.dummy_config, metadata.frame_count);
		source_frame_index = 0;
		emitted_frames = 1;

		spdlog::info(
			"dummy backend initialized: {}x{} fps={} frames={} startup_delay_ms={}",
			options.dummy_config.width,
			options.dummy_config.height,
			options.dummy_config.fps,
			options.dummy_config.frames,
			options.dummy_config.startup_delay_ms);

		on_metadata(metadata);
		on_frame(frame_buffer, metadata);

		worker_thread = std::jthread([this](std::stop_token stop_token) {
			worker_loop(stop_token);
		});
	}

	void worker_loop(std::stop_token stop_token) {
		const auto frame_interval = frame_interval_for(options.dummy_config);
		while (!stop_token.stop_requested()) {
			if (options.dummy_config.frames > 0) {
				uint32_t emitted_frames_snapshot = 0;
				{
					std::lock_guard lock(state_mutex);
					emitted_frames_snapshot = emitted_frames;
				}
				if (emitted_frames_snapshot >= options.dummy_config.frames) {
					spdlog::info(
						"dummy backend reached configured frame limit: {}",
						options.dummy_config.frames);
					on_error(ERR_EOS, "EOF");
					if (options.video_config.finite_stream_ending_behavior == app::FiniteStreamEndingBehavior::Loop) {
						continue;
					}
					break;
				}
			}

			std::this_thread::sleep_for(frame_interval);
			if (stop_token.stop_requested()) {
				break;
			}

			frame_metadata_t metadata_snapshot{};
			{
				std::lock_guard lock(state_mutex);
				source_frame_index += 1;
				metadata.frame_count += 1;
				metadata.timestamp_ns = timestamp_for_frame(metadata.frame_count);
				emitted_frames += 1;
				render_dummy_pattern(frame_buffer, options.dummy_config, source_frame_index);
				metadata_snapshot = metadata;
			}
			on_frame(frame_buffer, metadata_snapshot);
		}
	}

	void Shutdown() {
		if (worker_thread.joinable()) {
			worker_thread.request_stop();
			worker_thread.join();
		}
	}

	void SetOnMetadata(on_metadata_fn_t on_metadata_) {
		_on_metadata = std::move(on_metadata_);
	}

	void SetOnFrame(on_frame_fn_t on_frame_) {
		_on_frame = std::move(on_frame_);
	}

	void SetOnBodyTracking(on_body_tracking_fn_t) {}

	void SetOnError(on_error_fn_t on_error_) {
		_on_error = std::move(on_error_);
	}

	error_t ResetFrameCount() {
		std::lock_guard lock(state_mutex);
		metadata.frame_count = 0;
		metadata.timestamp_ns = timestamp_for_frame(metadata.frame_count);
		emitted_frames       = 0;
		source_frame_index = 0;
		return ERR_OK;
	}
};

DummyBackend::DummyBackend(app::DummyConfig dummy_config, const app::VideoConfig &video_config)
	: impl(std::make_unique<DummyBackendImpl>(DummyBackendOptions{
		.dummy_config = std::move(dummy_config),
		.video_config = video_config,
	})) {}

DummyBackend::~DummyBackend() = default;

void DummyBackend::Init() {
	impl->Init();
}

void DummyBackend::Shutdown() {
	impl->Shutdown();
}

void DummyBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void DummyBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

void DummyBackend::SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking) {
	impl->SetOnBodyTracking(std::move(on_body_tracking));
}

void DummyBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

source_info_t DummyBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
}

std::expected<seek_result_t, error_t> DummyBackend::SeekTimestampNs(uint64_t timestamp_ns) {
	return impl->SeekTimestampNs(timestamp_ns);
}

error_t DummyBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

std::expected<recording_status_t, error_t> DummyBackend::StartRecording(std::string_view) {
	return std::unexpected(-EOPNOTSUPP);
}

std::expected<recording_status_t, error_t> DummyBackend::StopRecording() {
	return std::unexpected(-EOPNOTSUPP);
}

std::expected<recording_status_t, error_t> DummyBackend::GetRecordingStatus() {
	return std::unexpected(-EOPNOTSUPP);
}

std::string DummyBackend::GetLastRecordingError() {
	return "recording is not supported by the dummy backend";
}

} // namespace app::backends
