#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <thread>
#include <chrono>
#include <optional>
#include <atomic>
#include <mutex>
#include <cvmmap/compat/expected.hpp>
#include <spdlog/spdlog.h>
#include <errno.h>
#include "app_backends_gst.hpp"
#include "app_backends_facade.hpp"
#include "app_enum_models.hpp"
#include "app_config.hpp"

namespace app::backends {
static constexpr const char *APP_SINK_NAME = "sink";

/// @brief Allowed caps for appsink - only accept BGR, RGB, BGRA, RGBA, or GRAY8 in video/x-raw format
/// @note This ensures we get raw video frames with known pixel formats that we can handle
/// @sa https://github.com/opencv/opencv/blob/4.x/modules/videoio/src/cap_gstreamer.cpp#L1620
static constexpr const char *ALLOWED_APPSINK_CAPS =
	"video/x-raw, format=(string){BGR, RGB}; "
	"video/x-raw, format=(string){BGRA, RGBA}; "
	"video/x-raw, format=(string){GRAY8}";

/// @brief Convert GStreamer video format to app::PixelFormat
/// @note Only supports formats allowed by ALLOWED_APPSINK_CAPS
static std::optional<PixelFormat> gst_format_to_pixel_format(GstVideoFormat format) {
	switch (format) {
	case GST_VIDEO_FORMAT_RGB:
		return PixelFormat::RGB;
	case GST_VIDEO_FORMAT_BGR:
		return PixelFormat::BGR;
	case GST_VIDEO_FORMAT_RGBA:
		return PixelFormat::RGBA;
	case GST_VIDEO_FORMAT_BGRA:
		return PixelFormat::BGRA;
	case GST_VIDEO_FORMAT_GRAY8:
		return PixelFormat::GRAY;
	default:
		// YUV and other formats are not supported - user must add videoconvert
		return std::nullopt;
	}
}

/// @brief Get number of channels for a pixel format
/// @note Only supports formats allowed by ALLOWED_APPSINK_CAPS
static uint8_t pixel_format_channels(PixelFormat fmt) {
	switch (fmt) {
	case PixelFormat::RGB:
	case PixelFormat::BGR:
		return 3;
	case PixelFormat::RGBA:
	case PixelFormat::BGRA:
		return 4;
	case PixelFormat::GRAY:
		return 1;
	default:
		// Should not happen if caps filtering is working correctly
		return 3;
	}
}

struct finite_source_info_t {
	double fps;
	int64_t duration_ns; // Duration in nanoseconds

	std::chrono::milliseconds frame_interval() const {
		return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(1000.0 / fps));
	}

	uint32_t estimated_frame_count() const {
		if (fps > 0 && duration_ns > 0) {
			return static_cast<uint32_t>((duration_ns / 1e9) * fps);
		}
		return 0;
	}
};

struct GStreamerBackendOptions {
	std::string pipeline;
	app::VideoConfig video_config;
};

struct GStreamerBackendImpl {
	GStreamerBackendOptions options;
	GstElement *pipeline{nullptr};
	GstElement *appsink{nullptr};
	std::jthread worker_thread;
	std::atomic<bool> initialized{false};

	on_metadata_fn_t _on_metadata{nullptr};
	on_frame_fn_t _on_frame{nullptr};
	on_error_fn_t _on_error{nullptr};

	// Frame metadata maintained by backend
	frame_metadata_t metadata{};
	std::optional<finite_source_info_t> finite_source_info{};
	GstVideoInfo video_info{};
	uint32_t source_frame_index{0};
	std::mutex state_mutex{};

	// Error tracking
	static constexpr int MAX_CONSECUTIVE_ERRORS = 3;
	int consecutive_errors{0};

	GStreamerBackendImpl() = default;
	explicit GStreamerBackendImpl(GStreamerBackendOptions opts) : options(std::move(opts)) {}

	~GStreamerBackendImpl() {
		Shutdown();
	}

	void on_metadata(const frame_metadata_t &metadata) {
		if (_on_metadata) {
			_on_metadata(metadata);
		}
	}

	void on_frame(std::span<uint8_t> frame_buffer, const frame_metadata_t &metadata) {
		if (_on_frame) {
			_on_frame(frame_buffer, metadata);
		}
	}

	void on_error(error_t error_code, std::string_view message) {
		if (_on_error) {
			_on_error(error_code, message);
		}
	}

	[[nodiscard]]
	uint64_t finite_frame_interval_ns() const {
		if (!finite_source_info || finite_source_info->fps <= 0.0) {
			return 0;
		}
		return static_cast<uint64_t>(1000000000.0 / finite_source_info->fps);
	}

	[[nodiscard]]
	uint64_t timestamp_for_source_frame(uint32_t frame_index) const {
		if (!finite_source_info) {
			return static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::system_clock::now().time_since_epoch())
					.count());
		}
		return static_cast<uint64_t>(frame_index) * finite_frame_interval_ns();
	}

	[[nodiscard]]
	bool effective_can_seek() const {
		return finite_source_info.has_value() &&
			   options.video_config.finite_source_can_seek();
	}

	[[nodiscard]]
	source_info_t GetSourceInfo() {
		std::lock_guard lock(state_mutex);
		source_info_t info{};
		if (finite_source_info) {
			info.source_kind       = cvmmap::SourceKind::Finite;
			info.timestamp_domain  = cvmmap::TimestampDomain::MediaTimeNs;
			info.timeline_start_ns = 0;
			info.timeline_end_ns   = static_cast<uint64_t>(std::max<int64_t>(
				finite_source_info->duration_ns - static_cast<int64_t>(finite_frame_interval_ns()),
				0));
			info.duration_ns       = static_cast<uint64_t>(finite_source_info->duration_ns);
			if (options.video_config.finite_source_can_seek()) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_SEEK;
			}
			if (options.video_config.finite_source_auto_loops()) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_AUTO_LOOP;
			}
		} else {
			info.source_kind      = cvmmap::SourceKind::Live;
			info.timestamp_domain = cvmmap::TimestampDomain::UnixEpochNs;
		}
		info.current_timestamp_ns = metadata.timestamp_ns;
		info.current_frame_count  = metadata.frame_count;
		return info;
	}

	/// @brief Ensure the pipeline ends with an appsink
	std::string ensure_appsink(const std::string &pipeline_str) {
		// Check if pipeline already contains appsink
		if (pipeline_str.find("appsink") != std::string::npos) {
			return pipeline_str;
		}
		// Append appsink with appropriate caps
		return pipeline_str + " ! appsink name=sink emit-signals=false sync=false";
	}

	/// @brief Check if this is a finite source (has duration)
	std::optional<finite_source_info_t> check_finite_source() {
		if (!pipeline) {
			return std::nullopt;
		}

		gint64 duration = 0;
		if (gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration) && duration > 0) {
			// Try to get framerate from video info
			double fps = 30.0; // Default fallback
			if (GST_VIDEO_INFO_FPS_N(&video_info) > 0 && GST_VIDEO_INFO_FPS_D(&video_info) > 0) {
				fps = static_cast<double>(GST_VIDEO_INFO_FPS_N(&video_info)) /
					  static_cast<double>(GST_VIDEO_INFO_FPS_D(&video_info));
			}
			return finite_source_info_t{
				.fps         = fps,
				.duration_ns = duration,
			};
		}
		return std::nullopt;
	}

	/// @brief Seek to the beginning of the stream
	bool seek_to_start() {
		if (!pipeline) {
			return false;
		}
		return gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
									   static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT), 0);
	}

	/// @brief Process a single sample from appsink
	/// @return true if sample was processed, false if EOS or error
	bool process_sample(GstSample *sample) {
		if (!sample) {
			return false;
		}

		GstBuffer *buffer = gst_sample_get_buffer(sample);
		if (!buffer) {
			gst_sample_unref(sample);
			return false;
		}

		GstMapInfo map;
		if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
			spdlog::error("maping GStreamer buffer");
			gst_sample_unref(sample);
			return false;
		}

		// Update frame count
		frame_metadata_t metadata_snapshot{};
		{
			std::lock_guard lock(state_mutex);
			source_frame_index += 1;
			metadata.frame_count += 1;
			metadata.timestamp_ns = timestamp_for_source_frame(source_frame_index);
			metadata_snapshot     = metadata;
		}

		// Invoke frame callback
		auto frame_buffer = std::span<uint8_t>(map.data, map.size);
		on_frame(frame_buffer, metadata_snapshot);

		gst_buffer_unmap(buffer, &map);
		gst_sample_unref(sample);
		return true;
	}

	void Init() {
		// Initialize GStreamer (safe to call multiple times)
		static std::once_flag gst_init_flag;
		std::call_once(gst_init_flag, []() {
			gst_init(nullptr, nullptr);
			spdlog::info("GStreamer initialized: {}", gst_version_string());
		});

		// Build pipeline string
		std::string pipeline_str = ensure_appsink(options.pipeline);
		spdlog::info("GStreamer pipeline: {}", pipeline_str);

		// Parse and create pipeline
		GError *error = nullptr;
		pipeline      = gst_parse_launch(pipeline_str.c_str(), &error);
		if (error) {
			spdlog::error("parsing GStreamer pipeline: {}", error->message);
			on_error(-EINVAL, error->message);
			g_error_free(error);
			return;
		}

		if (!pipeline) {
			spdlog::error("creating GStreamer pipeline");
			on_error(-ENODEV, "bad creation GStreamer pipeline");
			return;
		}

		// Get appsink element
		appsink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
		if (!appsink) {
			spdlog::error("finding appsink element named 'sink' in pipeline");
			on_error(-ENOENT, "find appsink in pipeline");
			gst_object_unref(pipeline);
			pipeline = nullptr;
			return;
		}

		// Configure appsink
		gst_app_sink_set_emit_signals(GST_APP_SINK(appsink), FALSE);
		gst_app_sink_set_drop(GST_APP_SINK(appsink), TRUE);
		gst_app_sink_set_max_buffers(GST_APP_SINK(appsink), 1);

		// Set caps filter to only accept BGR/RGB/GRAY8 formats
		// This ensures we get raw video with known pixel layouts
		GstCaps *desired_caps = gst_caps_from_string(ALLOWED_APPSINK_CAPS);
		if (!desired_caps) {
			spdlog::error("creating GStreamer caps from string");
			on_error(-EINVAL, "creation caps filter");
			gst_object_unref(appsink);
			gst_object_unref(pipeline);
			appsink  = nullptr;
			pipeline = nullptr;
			return;
		}

		// Check if upstream can provide the formats we want
		GstPad *sink_pad = gst_element_get_static_pad(appsink, APP_SINK_NAME);
		if (sink_pad) {
			GstCaps *peer_caps = gst_pad_peer_query_caps(sink_pad, nullptr);
			if (peer_caps) {
				if (!gst_caps_can_intersect(desired_caps, peer_caps)) {
					// Log what formats are available vs what we want
					gchar *peer_caps_str = gst_caps_to_string(peer_caps);
					spdlog::warn("upstream cannot provide required formats. "
								 "Required: BGR/RGB/BGRA/RGBA/GRAY8, Available: {}",
								 peer_caps_str ? peer_caps_str : "unknown");
					spdlog::warn("consider adding 'videoconvert' element before appsink");
					g_free(peer_caps_str);
				}
				gst_caps_unref(peer_caps);
			}
			gst_object_unref(sink_pad);
		}

		// Apply caps filter to appsink
		gst_app_sink_set_caps(GST_APP_SINK(appsink), desired_caps);
		spdlog::info("appsink caps filter set: {}", ALLOWED_APPSINK_CAPS);
		gst_caps_unref(desired_caps);

		// Start pipeline
		GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
		if (ret == GST_STATE_CHANGE_FAILURE) {
			spdlog::error("starting GStreamer pipeline");
			on_error(-EIO, "bad start GStreamer pipeline");
			gst_object_unref(appsink);
			gst_object_unref(pipeline);
			appsink  = nullptr;
			pipeline = nullptr;
			return;
		}

		// Wait for pipeline to be ready and pull first sample to get metadata
		GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(appsink));
		if (!sample) {
			spdlog::error("pulling first sample from GStreamer pipeline");
			on_error(-EIO, "Failed to capture first frame");
			gst_element_set_state(pipeline, GST_STATE_NULL);
			gst_object_unref(appsink);
			gst_object_unref(pipeline);
			appsink  = nullptr;
			pipeline = nullptr;
			return;
		}

		// Extract video info from caps
		GstCaps *caps = gst_sample_get_caps(sample);
		if (caps && gst_video_info_from_caps(&video_info, caps)) {
			GstVideoFormat format = GST_VIDEO_INFO_FORMAT(&video_info);
			auto pixel_format     = gst_format_to_pixel_format(format);

			// Validate that we got a supported format
			if (!pixel_format) {
				gchar *caps_str = gst_caps_to_string(caps);
				spdlog::error("unsupported GStreamer video format: {}. "
							  "Only BGR, RGB, BGRA, RGBA, GRAY8 are supported. "
							  "Consider adding 'videoconvert ! video/x-raw,format=BGR' to your pipeline. "
							  "Received caps: {}",
							  gst_video_format_to_string(format),
							  caps_str ? caps_str : "unknown");
				g_free(caps_str);
				on_error(-EINVAL, "Unsupported video format");
				gst_sample_unref(sample);
				gst_element_set_state(pipeline, GST_STATE_NULL);
				gst_object_unref(appsink);
				gst_object_unref(pipeline);
				appsink  = nullptr;
				pipeline = nullptr;
				return;
			}

			uint32_t width    = GST_VIDEO_INFO_WIDTH(&video_info);
			uint32_t height   = GST_VIDEO_INFO_HEIGHT(&video_info);
			uint8_t channels  = pixel_format_channels(*pixel_format);
			uint32_t buf_size = GST_VIDEO_INFO_SIZE(&video_info);

			metadata.frame_count = 0;
			metadata.info        = frame_info_t{
				.width        = static_cast<uint16_t>(width),
				.height       = static_cast<uint16_t>(height),
				.channels     = channels,
				.depth        = Depth::U8, // GStreamer video/x-raw uses 8-bit per channel
				.pixel_format = *pixel_format,
				.buffer_size  = buf_size,
			};

			spdlog::info("GStreamer frame info: {}x{}x{}; format={}; bufferSize={}; pixelFormat={}",
						 width, height, channels,
						 gst_video_format_to_string(format),
						 buf_size,
						 to_str(*pixel_format));
		} else {
			gchar *caps_str = caps ? gst_caps_to_string(caps) : nullptr;
			spdlog::error("could not extract video info from caps. "
						  "Expected video/x-raw format. Received: {}",
						  caps_str ? caps_str : "no caps");
			g_free(caps_str);
			on_error(-EINVAL, "Invalid video caps - expected video/x-raw");
			gst_sample_unref(sample);
			gst_element_set_state(pipeline, GST_STATE_NULL);
			gst_object_unref(appsink);
			gst_object_unref(pipeline);
			appsink  = nullptr;
			pipeline = nullptr;
			return;
		}

		// Check for finite source
		finite_source_info = check_finite_source();
		if (finite_source_info) {
			spdlog::info("detected finite source; fps={} ({}ms), duration={}s, estimated_frames={}, auto_loops={}, can_seek={}",
						 finite_source_info->fps,
						 finite_source_info->frame_interval().count(),
						 finite_source_info->duration_ns / 1e9,
						 finite_source_info->estimated_frame_count(),
						 options.video_config.finite_source_auto_loops(),
						 options.video_config.finite_source_can_seek());
		} else {
			spdlog::info("infinite source detected (live stream)");
		}

		// Invoke metadata callback
		on_metadata(metadata);

		// Process first sample
		source_frame_index    = 0;
		metadata.timestamp_ns = timestamp_for_source_frame(source_frame_index);
		process_sample(sample);

		initialized = true;

		// Start worker thread
		worker_thread = std::jthread([this](std::stop_token stop_token) {
			worker_loop(stop_token);
		});
	}

	void worker_loop(std::stop_token stop_token) {
		while (!stop_token.stop_requested()) {
			// Check for bus messages (errors, EOS, etc.)
			GstBus *bus     = gst_element_get_bus(pipeline);
			GstMessage *msg = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
			gst_object_unref(bus);

			if (msg) {
				switch (GST_MESSAGE_TYPE(msg)) {
				case GST_MESSAGE_ERROR: {
					GError *err = nullptr;
					gchar *debug_info;
					gst_message_parse_error(msg, &err, &debug_info);
					spdlog::error("GStreamer issue: {} ({})", err->message, debug_info ? debug_info : "none");
					on_error(-EIO, err->message);
					g_clear_error(&err);
					g_free(debug_info);
					gst_message_unref(msg);
					return;
				}
				case GST_MESSAGE_EOS: {
					spdlog::info("GStreamer end-of-stream");
					if (finite_source_info &&
						options.video_config.finite_source_loops_silently()) {
						spdlog::info("looping finite source");
						if (!seek_to_start()) {
							spdlog::error("seeking to start for looping");
							on_error(-EIO, "bad loop video");
							gst_message_unref(msg);
							return;
						}
						gst_message_unref(msg);
						continue;
					} else {
						on_error(0, "EOF");
						gst_message_unref(msg);
						return;
					}
				}
				default:
					break;
				}
				gst_message_unref(msg);
			}

			// Pull sample from appsink with timeout
			GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), GST_MSECOND * 100);
			if (sample) {
				consecutive_errors = 0;
				if (!process_sample(sample)) {
					consecutive_errors++;
					if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
						spdlog::error("too many consecutive bad frame-processing results");
						on_error(-EIO, "Too many frame processing errors");
						return;
					}
				}

				spdlog::debug("frame@{}", metadata.frame_count);
			} else {
				// Check if appsink is EOS
				if (gst_app_sink_is_eos(GST_APP_SINK(appsink))) {
					spdlog::info("appsink reached EOS");
					if (finite_source_info &&
						options.video_config.finite_source_loops_silently()) {
						if (!seek_to_start()) {
							spdlog::error("seeking to start for looping");
							on_error(-EIO, "bad loop video");
							return;
						}
						continue;
					} else {
						on_error(ERR_EOS, "EOF");
						if (options.video_config.finite_stream_ending_behavior == app::FiniteStreamEndingBehavior::Loop) {
							continue;
						}
						return;
					}
				}
				// No sample available, continue polling
			}
		}
	}

	void Shutdown() {
		if (worker_thread.joinable()) {
			worker_thread.request_stop();
			worker_thread.join();
		}

		if (pipeline) {
			gst_element_set_state(pipeline, GST_STATE_NULL);
		}

		if (appsink) {
			gst_object_unref(appsink);
			appsink = nullptr;
		}

		if (pipeline) {
			gst_object_unref(pipeline);
			pipeline = nullptr;
		}

		initialized = false;
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

	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns) {
		if (!finite_source_info) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}
		if (!effective_can_seek()) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}
		if (!pipeline) {
			return cvmmap::unexpected(-ENODEV);
		}

		const auto duration = static_cast<uint64_t>(finite_source_info->duration_ns);
		if (timestamp_ns > duration) {
			return cvmmap::unexpected(-ERANGE);
		}

		const auto interval_ns         = finite_frame_interval_ns();
		const auto frame_index         = interval_ns == 0 ? 0u : static_cast<uint32_t>((timestamp_ns + interval_ns - 1) / interval_ns);
		const auto landed_timestamp_ns = interval_ns == 0 ? timestamp_ns : static_cast<uint64_t>(frame_index) * interval_ns;

		std::lock_guard lock(state_mutex);
		bool success = gst_element_seek_simple(
			pipeline, GST_FORMAT_TIME,
			static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
			static_cast<gint64>(timestamp_ns));
		if (!success) {
			return cvmmap::unexpected(-EIO);
		}

		source_frame_index    = frame_index;
		metadata.frame_count  = 0;
		metadata.timestamp_ns = landed_timestamp_ns;
		return seek_result_t{
			.requested_timestamp_ns = timestamp_ns,
			.landed_timestamp_ns    = landed_timestamp_ns,
			.landed_frame_count     = metadata.frame_count,
			.exact_match            = landed_timestamp_ns == timestamp_ns,
		};
	}

	error_t ResetFrameCount() {
		std::lock_guard lock(state_mutex);
		if (finite_source_info && options.video_config.finite_source_can_seek()) {
			// Finite source: seek to beginning
			if (!pipeline) {
				return -ENODEV;
			}
			if (!seek_to_start()) {
				return -EIO;
			}
		}
		// Reset internal frame count for both finite and stream sources
		metadata.frame_count  = 0;
		source_frame_index    = 0;
		metadata.timestamp_ns = timestamp_for_source_frame(source_frame_index);
		return 0;
	}
};

// GStreamerBackend public API

GStreamerBackend::GStreamerBackend(std::string pipeline, const app::VideoConfig &video_config)
	: impl(std::make_unique<GStreamerBackendImpl>(GStreamerBackendOptions{
		  .pipeline     = std::move(pipeline),
		  .video_config = video_config,
	  })) {}

GStreamerBackend::~GStreamerBackend() = default;

GStreamerBackend::GStreamerBackend(GStreamerBackend &&) noexcept            = default;
GStreamerBackend &GStreamerBackend::operator=(GStreamerBackend &&) noexcept = default;

void GStreamerBackend::Init() {
	impl->Init();
}

void GStreamerBackend::Shutdown() {
	impl->Shutdown();
}

void GStreamerBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void GStreamerBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

void GStreamerBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

source_info_t GStreamerBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
}

cvmmap::expected<seek_result_t, error_t> GStreamerBackend::SeekTimestampNs(uint64_t timestamp_ns) {
	return impl->SeekTimestampNs(timestamp_ns);
}

error_t GStreamerBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

} // namespace app::backends
