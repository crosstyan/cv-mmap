#include <bits/types/error_t.h>
#include <opencv2/videoio.hpp>
#include <thread>
#include <chrono>
#include <optional>
#include <regex>
#include <mutex>
#include <cvmmap/compat/expected.hpp>
#include <spdlog/spdlog.h>
#include <errno.h>
#include "app_backends_opencv.hpp"
#include "app_backends_facade.hpp"
#include "app_enum_models.hpp"
#include "app_config.hpp"

namespace app::backends {

struct finite_source_info_t {
	double fps;
	uint32_t frame_count;

	std::chrono::milliseconds frame_interval() const {
		return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(1000.0 / fps));
	}
};

struct OpenCVBackendOptions {
	opencv_parameter_t parameter;
	app::VideoConfig video_config;
	cv::VideoCaptureAPIs api_preference;
};

struct OpenCVBackendImpl {
	OpenCVBackendOptions options;
	cv::VideoCapture cap;
	std::jthread worker_thread;
	on_metadata_fn_t _on_metadata{nullptr};
	on_frame_fn_t _on_frame{nullptr};
	on_error_fn_t _on_error{nullptr};

	// Frame metadata maintained by backend
	frame_metadata_t metadata{};
	std::optional<finite_source_info_t> finite_source_info{};
	uint32_t source_frame_index{0};
	std::mutex state_mutex{};

	// Error tracking for live sources
	static constexpr int MAX_CONSECUTIVE_EMPTY_FRAMES = 3;
	int consecutive_empty_frames{0};

	OpenCVBackendImpl() = default;
	OpenCVBackendImpl(OpenCVBackendOptions opts) : options(std::move(opts)) {}

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
	source_info_t GetSourceInfo() {
		std::lock_guard lock(state_mutex);
		source_info_t info{};
		if (finite_source_info) {
			info.source_kind = cvmmap::SourceKind::Finite;
			info.timestamp_domain = cvmmap::TimestampDomain::MediaTimeNs;
			info.timeline_start_ns = 0;
			info.timeline_end_ns =
				static_cast<uint64_t>(finite_source_info->frame_count - 1) *
				finite_frame_interval_ns();
			info.duration_ns =
				static_cast<uint64_t>(finite_source_info->frame_count) *
				finite_frame_interval_ns();
			if (!options.video_config.use_finite_as_infinite_stream) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_SEEK;
			}
			if (options.video_config.finite_stream_ending_behavior ==
					app::FiniteStreamEndingBehavior::Loop ||
				options.video_config.use_finite_as_infinite_stream) {
				info.flags |= cvmmap::SOURCE_INFO_FLAG_AUTO_LOOP;
			}
		} else {
			info.source_kind = cvmmap::SourceKind::Live;
			info.timestamp_domain = cvmmap::TimestampDomain::UnixEpochNs;
		}
		info.current_timestamp_ns = metadata.timestamp_ns;
		info.current_frame_count = metadata.frame_count;
		return info;
	}

	std::optional<finite_source_info_t> check_finite_source() {
		const auto fps         = cap.get(cv::CAP_PROP_FPS);
		const auto frame_count = cap.get(cv::CAP_PROP_FRAME_COUNT);
		if (fps > 0 and frame_count > 0) {
			return finite_source_info_t{
				.fps         = fps,
				.frame_count = static_cast<uint32_t>(frame_count),
			};
		}
		return std::nullopt;
	}

	void reset_video_position() {
		cap.set(cv::CAP_PROP_POS_FRAMES, 0);
	}

	int get_video_position() {
		return static_cast<int>(cap.get(cv::CAP_PROP_POS_FRAMES));
	}

	void check_gst_pipeline(const std::string &pipeline) {
		std::regex re(R"(\,\s+)");
		std::smatch m;
		if (std::regex_search(pipeline, m, re)) {
			spdlog::warn("extra spaces found in the pipeline string: `{}`. "
						 "GStreamer won't happy about extra space in caps. "
						 "please remove them, otherwise it may cause unexpected behavior.",
						 pipeline);
		}
	}

	void Init() {
		// Open video capture
		if (std::holds_alternative<std::string>(options.parameter)) {
			const auto pipeline = std::get<std::string>(options.parameter);
			if (options.api_preference == cv::CAP_GSTREAMER) {
				check_gst_pipeline(pipeline);
			}
			spdlog::info("open video source pipeline (string): {}", pipeline);
			cap.open(pipeline, options.api_preference);
		} else {
			const auto index = std::get<int>(options.parameter);
			spdlog::info("open video source index (int): {}", index);
			cap.open(index, options.api_preference);
		}

		if (not cap.isOpened()) {
			spdlog::error("open video source. check OpenCV VideoCapture API support if you're sure the source is correct.");
			spdlog::info("OpenCV build information:\n{}", cv::getBuildInformation());
			on_error(-ENODEV, "Failed to open video source");
			return;
		}

		// Check for finite source
		finite_source_info = check_finite_source();
		if (finite_source_info) {
			spdlog::info("detected finite source; fps={} ({}ms), frame_count={}, is_loop={}",
						 finite_source_info->fps,
						 finite_source_info->frame_interval().count(),
						 finite_source_info->frame_count,
						 options.video_config.use_finite_as_infinite_stream);
		} else {
			spdlog::info("infinite source detected (live stream)");
		}

		// Capture first frame to get metadata
		cv::Mat frame;
		cap >> frame;
		if (frame.empty()) {
			spdlog::error("capture first frame");
			on_error(-EIO, "bad capture first frame");
			return;
		}

		// Build metadata from first frame
		const auto pixel_format = app::guess_pixel_format(frame.channels());
		metadata.frame_count    = 0;
		metadata.info           = frame_info_t{
					  .width        = static_cast<uint16_t>(frame.cols),
					  .height       = static_cast<uint16_t>(frame.rows),
					  .channels     = static_cast<uint8_t>(frame.channels()),
					  .depth        = static_cast<Depth>(frame.depth()),
					  .pixel_format = pixel_format,
					  .buffer_size  = static_cast<uint32_t>(frame.total() * frame.elemSize()),
        };

		spdlog::info("initial frame info: {}x{}x{}; "
					 "depth={}({}); "
					 "stride[0]={}; "
					 "stride[1]={}; "
					 "total={}; "
					 "elemSize={}; "
					 "bufferSize={}; "
					 "pixelFormat={};",
					 frame.cols,
					 frame.rows,
					 frame.channels(),
					 app::to_str(static_cast<app::Depth>(frame.depth())),
					 frame.depth(),
					 frame.step[0],
					 frame.step[1],
					 frame.total(),
					 frame.elemSize(),
					 frame.total() * frame.elemSize(),
					 app::to_str(pixel_format));

		// Invoke metadata callback
		on_metadata(metadata);

		// Invoke frame callback for first frame
		source_frame_index = 0;
		metadata.timestamp_ns = timestamp_for_source_frame(source_frame_index);
		auto frame_buffer = std::span<uint8_t>(frame.data, frame.total() * frame.elemSize());
		on_frame(frame_buffer, metadata);

		// Start worker thread
		worker_thread = std::jthread([this](std::stop_token stop_token) {
			worker_loop(stop_token);
		});
	}

	void worker_loop(std::stop_token stop_token) {
		cv::Mat frame;
		while (!stop_token.stop_requested()) {
			{
				std::lock_guard lock(state_mutex);
				cap >> frame;
			}
			if (frame.empty()) {
				if (finite_source_info) {
					spdlog::info("reached end of finite video source");
					if (options.video_config.use_finite_as_infinite_stream) {
						reset_video_position();
						consecutive_empty_frames = 0;
						continue;
					} else {
						// End of non-looping finite source
						on_error(ERR_EOS, "EOF");
						if (options.video_config.finite_stream_ending_behavior == app::FiniteStreamEndingBehavior::Loop) {
							continue;
						}
						break;
					}
				} else {
					// Live source - track consecutive empty frames
					consecutive_empty_frames++;
					if (consecutive_empty_frames >= MAX_CONSECUTIVE_EMPTY_FRAMES) {
						spdlog::error("live source: {} consecutive empty frames, treating as device error",
									  consecutive_empty_frames);
						on_error(-EIO, "Device disconnected or capture failure");
						break;
					}
					spdlog::warn("live source empty frame captured ({}/{})",
								 consecutive_empty_frames, MAX_CONSECUTIVE_EMPTY_FRAMES);
					continue;
				}
			}

			// Reset empty frame counter on successful capture
			consecutive_empty_frames = 0;

			frame_metadata_t metadata_snapshot{};
			{
				std::lock_guard lock(state_mutex);
				source_frame_index += 1;
				metadata.frame_count += 1;
				metadata.timestamp_ns = timestamp_for_source_frame(source_frame_index);
				metadata_snapshot = metadata;
			}

			// Invoke frame callback
			auto frame_buffer = std::span<uint8_t>(frame.data, frame.total() * frame.elemSize());
			on_frame(frame_buffer, metadata_snapshot);

			// Log and sleep for finite sources
			if (finite_source_info) {
				const auto current = get_video_position();
				spdlog::debug("frame@{} ({}/{})", metadata.frame_count, current, finite_source_info->frame_count);
				std::this_thread::sleep_for(finite_source_info->frame_interval());
			} else {
				spdlog::debug("frame@{}", metadata.frame_count);
			}
		}
	}

	void Shutdown() {
		if (worker_thread.joinable()) {
			worker_thread.request_stop();
			worker_thread.join();
		}
		if (cap.isOpened()) {
			cap.release();
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

	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns) {
		if (!finite_source_info) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}
		if (options.video_config.use_finite_as_infinite_stream) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}
		const auto interval_ns = finite_frame_interval_ns();
		const auto max_timestamp_ns =
			static_cast<uint64_t>(finite_source_info->frame_count - 1) *
			interval_ns;
		if (timestamp_ns > max_timestamp_ns) {
			return cvmmap::unexpected(-ERANGE);
		}

		const auto frame_index = static_cast<uint32_t>(
			(timestamp_ns + interval_ns - 1) / interval_ns);
		std::lock_guard lock(state_mutex);
		bool success =
			cap.set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(frame_index));
		if (!success) {
			return cvmmap::unexpected(-EIO);
		}
		source_frame_index = frame_index;
		metadata.frame_count = 0;
		metadata.timestamp_ns = timestamp_for_source_frame(source_frame_index);
		return seek_result_t{
			.requested_timestamp_ns = timestamp_ns,
			.landed_timestamp_ns = metadata.timestamp_ns,
			.landed_frame_count = metadata.frame_count,
			.exact_match = (metadata.timestamp_ns == timestamp_ns),
		};
	}

	error_t ResetFrameCount() {
		std::lock_guard lock(state_mutex);
		if (finite_source_info && !options.video_config.use_finite_as_infinite_stream) {
			// Finite source: seek to beginning
			bool success = cap.set(cv::CAP_PROP_POS_FRAMES, 0);
			if (!success) {
				return -EIO;
			}
		}
		// Reset internal frame count for both finite and stream sources
		metadata.frame_count = 0;
		source_frame_index = 0;
		metadata.timestamp_ns = timestamp_for_source_frame(source_frame_index);
		return 0;
	}
};

// OpenCVBackend public API

OpenCVBackend::OpenCVBackend(std::variant<std::string, int> parameter,
							 const app::VideoConfig &video_config,
							 app::VideoCaptureAPIs api_preference) : impl(std::make_unique<OpenCVBackendImpl>(OpenCVBackendOptions{
																		 .parameter      = std::move(parameter),
																		 .video_config   = video_config,
																		 .api_preference = static_cast<cv::VideoCaptureAPIs>(api_preference),
																	 })) {}
OpenCVBackend::~OpenCVBackend() = default;
void OpenCVBackend::Init() {
	impl->Init();
}

void OpenCVBackend::Shutdown() {
	impl->Shutdown();
}

void OpenCVBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void OpenCVBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

void OpenCVBackend::SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking) {
	impl->SetOnBodyTracking(std::move(on_body_tracking));
}

void OpenCVBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

source_info_t OpenCVBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
}

cvmmap::expected<seek_result_t, error_t> OpenCVBackend::SeekTimestampNs(uint64_t timestamp_ns) {
	return impl->SeekTimestampNs(timestamp_ns);
}

error_t OpenCVBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

cvmmap::expected<recording_status_t, error_t> OpenCVBackend::StartRecording(std::string_view) {
	return cvmmap::unexpected(-EOPNOTSUPP);
}

cvmmap::expected<recording_status_t, error_t> OpenCVBackend::StopRecording() {
	return cvmmap::unexpected(-EOPNOTSUPP);
}

cvmmap::expected<recording_status_t, error_t> OpenCVBackend::GetRecordingStatus() {
	return cvmmap::unexpected(-EOPNOTSUPP);
}

std::string OpenCVBackend::GetLastRecordingError() {
	return "recording is not supported by the OpenCV backend";
}
}
