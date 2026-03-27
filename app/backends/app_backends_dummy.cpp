#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <cvmmap/compat/expected.hpp>
#include <spdlog/spdlog.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include "app_backends_dummy.hpp"
#include "app_backends_facade.hpp"
#include "app_config.hpp"
#include "app_enum_models.hpp"

namespace app::backends {

namespace {

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

	struct DummyOverlayFont {
		std::vector<unsigned char> bytes{};
		stbtt_fontinfo info{};
		float scale{1.0f};
		int ascent{0};
		int descent{0};
		int line_gap{0};
		int line_height_px{0};
	};

	std::filesystem::path default_dummy_overlay_font_path() {
		return std::filesystem::path(CVMMAP_DUMMY_DEFAULT_FONT_PATH);
	}

	std::vector<unsigned char> read_binary_file(const std::filesystem::path &path) {
		std::ifstream stream(path, std::ios::binary);
		if (!stream) {
			throw std::runtime_error("failed to open dummy overlay font: " + path.string());
		}

		stream.seekg(0, std::ios::end);
		const auto size = stream.tellg();
		if (size <= 0) {
			throw std::runtime_error("dummy overlay font is empty: " + path.string());
		}
		stream.seekg(0, std::ios::beg);

		std::vector<unsigned char> bytes(static_cast<size_t>(size));
		if (!stream.read(reinterpret_cast<char *>(bytes.data()), size)) {
			throw std::runtime_error("failed to read dummy overlay font: " + path.string());
		}
		return bytes;
	}

	DummyOverlayFont load_dummy_overlay_font(const app::DummyConfig &config) {
		const auto path   = config.timestamp_overlay_font_path
								? std::filesystem::path(*config.timestamp_overlay_font_path)
								: default_dummy_overlay_font_path();
		auto bytes        = read_binary_file(path);
		const auto offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
		if (offset < 0) {
			throw std::runtime_error("invalid dummy overlay font: " + path.string());
		}

		DummyOverlayFont font{};
		font.bytes = std::move(bytes);
		if (stbtt_InitFont(&font.info, font.bytes.data(), offset) == 0) {
			throw std::runtime_error("failed to initialize dummy overlay font: " + path.string());
		}

		font.scale = stbtt_ScaleForPixelHeight(&font.info, 18.0f);
		stbtt_GetFontVMetrics(&font.info, &font.ascent, &font.descent, &font.line_gap);
		font.line_height_px = std::max(
			1,
			static_cast<int>(std::ceil((font.ascent - font.descent + font.line_gap) * font.scale)));
		return font;
	}

	void fill_rect(
		std::span<uint8_t> frame_buffer,
		const int frame_width,
		const int frame_height,
		const int x0,
		const int y0,
		const int width,
		const int height,
		const std::array<uint8_t, 3> &color) {
		const auto x_begin = std::max(0, x0);
		const auto y_begin = std::max(0, y0);
		const auto x_end   = std::min(frame_width, x0 + width);
		const auto y_end   = std::min(frame_height, y0 + height);

		for (int y = y_begin; y < y_end; ++y) {
			for (int x = x_begin; x < x_end; ++x) {
				const auto pixel_index        = static_cast<size_t>((y * frame_width + x) * 3);
				frame_buffer[pixel_index + 0] = color[0];
				frame_buffer[pixel_index + 1] = color[1];
				frame_buffer[pixel_index + 2] = color[2];
			}
		}
	}

	void blend_glyph_bitmap(
		std::span<uint8_t> frame_buffer,
		const int frame_width,
		const int frame_height,
		const int origin_x,
		const int origin_y,
		const unsigned char *bitmap,
		const int bitmap_width,
		const int bitmap_height,
		const std::array<uint8_t, 3> &color) {
		for (int glyph_y = 0; glyph_y < bitmap_height; ++glyph_y) {
			const auto frame_y = origin_y + glyph_y;
			if (frame_y < 0 || frame_y >= frame_height) {
				continue;
			}
			for (int glyph_x = 0; glyph_x < bitmap_width; ++glyph_x) {
				const auto frame_x = origin_x + glyph_x;
				if (frame_x < 0 || frame_x >= frame_width) {
					continue;
				}

				const auto alpha = static_cast<uint16_t>(bitmap[glyph_y * bitmap_width + glyph_x]);
				if (alpha == 0) {
					continue;
				}

				const auto pixel_index = static_cast<size_t>((frame_y * frame_width + frame_x) * 3);
				for (size_t channel = 0; channel < 3; ++channel) {
					const auto dst                      = static_cast<uint16_t>(frame_buffer[pixel_index + channel]);
					const auto src                      = static_cast<uint16_t>(color[channel]);
					frame_buffer[pixel_index + channel] = static_cast<uint8_t>(
						(dst * (255u - alpha) + src * alpha) / 255u);
				}
			}
		}
	}

	void draw_text_line(
		std::span<uint8_t> frame_buffer,
		const int frame_width,
		const int frame_height,
		const DummyOverlayFont &font,
		const int origin_x,
		const int origin_y,
		std::string_view text,
		const std::array<uint8_t, 3> &color) {
		float pen_x            = static_cast<float>(origin_x);
		const float baseline_y = static_cast<float>(origin_y) + font.ascent * font.scale;
		int previous_codepoint = 0;

		for (const unsigned char ch : text) {
			const int codepoint = static_cast<int>(ch);
			if (codepoint < 32 || codepoint > 126) {
				continue;
			}

			if (previous_codepoint != 0) {
				pen_x += font.scale * static_cast<float>(
										  stbtt_GetCodepointKernAdvance(&font.info, previous_codepoint, codepoint));
			}

			int advance_width     = 0;
			int left_side_bearing = 0;
			stbtt_GetCodepointHMetrics(&font.info, codepoint, &advance_width, &left_side_bearing);
			(void)left_side_bearing;

			int glyph_width             = 0;
			int glyph_height            = 0;
			int glyph_x_offset          = 0;
			int glyph_y_offset          = 0;
			unsigned char *glyph_bitmap = stbtt_GetCodepointBitmap(
				&font.info,
				0.0f,
				font.scale,
				codepoint,
				&glyph_width,
				&glyph_height,
				&glyph_x_offset,
				&glyph_y_offset);

			if (glyph_bitmap != nullptr) {
				blend_glyph_bitmap(
					frame_buffer,
					frame_width,
					frame_height,
					static_cast<int>(std::floor(pen_x)) + glyph_x_offset,
					static_cast<int>(std::floor(baseline_y)) + glyph_y_offset,
					glyph_bitmap,
					glyph_width,
					glyph_height,
					color);
				stbtt_FreeBitmap(glyph_bitmap, nullptr);
			}

			pen_x += font.scale * static_cast<float>(advance_width);
			previous_codepoint = codepoint;
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
	DummyOverlayFont overlay_font{};
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
	std::string timestamp_domain_label() const {
		return is_finite_source() ? "media_ns" : "unix_ns";
	}

	void render_overlay(const frame_metadata_t &metadata_snapshot) {
		const int frame_width  = options.dummy_config.width;
		const int frame_height = options.dummy_config.height;
		const int padding      = 4;
		const int line_spacing = 2;
		const std::array<std::string, 3> lines{
			"frame: " + std::to_string(metadata_snapshot.frame_count),
			"timestamp_ns: " + std::to_string(metadata_snapshot.timestamp_ns),
			"domain: " + timestamp_domain_label(),
		};

		const auto line_block_height = static_cast<int>(lines.size()) * overlay_font.line_height_px +
									   static_cast<int>(lines.size() - 1) * line_spacing;
		const auto overlay_width     = std::min(frame_width, 320);
		const auto overlay_height    = std::min(frame_height, line_block_height + padding * 2);

		fill_rect(
			frame_buffer,
			frame_width,
			frame_height,
			0,
			0,
			overlay_width,
			overlay_height,
			{0, 0, 0});

		int text_y = padding;
		for (const auto &line : lines) {
			draw_text_line(
				frame_buffer,
				frame_width,
				frame_height,
				overlay_font,
				padding,
				text_y,
				line,
				{255, 255, 255});
			text_y += overlay_font.line_height_px + line_spacing;
		}
	}

	void render_current_frame_locked() {
		render_dummy_pattern(frame_buffer, options.dummy_config, source_frame_index);
		render_overlay(metadata);
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
			if (options.video_config.finite_source_can_seek()) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_SEEK;
			}
			if (options.video_config.finite_source_auto_loops()) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_AUTO_LOOP;
			}
			if (options.video_config.finite_source_loop_emits_reset()) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_LOOP_EMITS_RESET;
			}
		}
		info.current_timestamp_ns = metadata.timestamp_ns;
		info.current_frame_count  = metadata.frame_count;
		return info;
	}

	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(
		uint64_t timestamp_ns) {
		if (!is_finite_source() ||
			!options.video_config.finite_source_can_seek()) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}

		const auto interval_ns = frame_interval_ns_for(options.dummy_config);
		const auto max_timestamp_ns =
			static_cast<uint64_t>(std::max<uint32_t>(
				options.dummy_config.frames - 1, 0u)) *
			interval_ns;
		if (timestamp_ns > max_timestamp_ns) {
			return cvmmap::unexpected(-ERANGE);
		}

		const auto frame_index = static_cast<uint32_t>(timestamp_ns / interval_ns);

		frame_metadata_t metadata_snapshot{};
		{
			std::lock_guard lock(state_mutex);
			metadata.frame_count  = 0;
			metadata.timestamp_ns = frame_index * interval_ns;
			emitted_frames        = frame_index + 1;
			source_frame_index    = frame_index;
			render_current_frame_locked();
			metadata_snapshot = metadata;
		}
		on_frame(frame_buffer, metadata_snapshot);
		return seek_result_t{
			.requested_timestamp_ns = timestamp_ns,
			.landed_timestamp_ns    = metadata_snapshot.timestamp_ns,
			.landed_frame_count     = metadata_snapshot.frame_count,
			.exact_match            = (timestamp_ns % interval_ns) == 0,
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

		overlay_font = load_dummy_overlay_font(options.dummy_config);
		frame_buffer.resize(metadata.info.buffer_size);
		source_frame_index = 0;
		emitted_frames     = 1;
		render_current_frame_locked();

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
					if (options.video_config.finite_source_loops_silently()) {
						frame_metadata_t metadata_snapshot{};
						{
							std::lock_guard lock(state_mutex);
							metadata.frame_count  = 0;
							metadata.timestamp_ns = timestamp_for_frame(metadata.frame_count);
							source_frame_index    = 0;
							emitted_frames        = 1;
							render_current_frame_locked();
							metadata_snapshot = metadata;
						}
						on_frame(frame_buffer, metadata_snapshot);
						continue;
					}

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
				render_current_frame_locked();
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

	void SetOnError(on_error_fn_t on_error_) {
		_on_error = std::move(on_error_);
	}

	error_t ResetFrameCount() {
		frame_metadata_t metadata_snapshot{};
		{
			std::lock_guard lock(state_mutex);
			metadata.frame_count  = 0;
			metadata.timestamp_ns = timestamp_for_frame(metadata.frame_count);
			emitted_frames        = 1;
			source_frame_index    = 0;
			render_current_frame_locked();
			metadata_snapshot = metadata;
		}
		on_frame(frame_buffer, metadata_snapshot);
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

void DummyBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

source_info_t DummyBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
}

cvmmap::expected<seek_result_t, error_t> DummyBackend::SeekTimestampNs(uint64_t timestamp_ns) {
	return impl->SeekTimestampNs(timestamp_ns);
}

error_t DummyBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

} // namespace app::backends
