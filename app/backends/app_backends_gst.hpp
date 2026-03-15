#ifndef EE8D58F7_CFE6_44F9_8A1A_BAF66882A291
#define EE8D58F7_CFE6_44F9_8A1A_BAF66882A291
#include <memory>
#include <string>
#include "app_backends_facade.hpp"

namespace app {
struct VideoConfig;
}

namespace app::backends {

struct GStreamerBackendImpl;

/// @brief GStreamer backend for video capture using gst-launch-1.0 style pipelines
///
/// This backend accepts a GStreamer pipeline string and uses appsink to receive frames.
/// The output format is restricted to video/x-raw with BGR, RGB, BGRA, RGBA, or GRAY8 formats.
///
/// @note The pipeline should end with `! appsink name=sink` or similar.
///       If appsink is not present, it will be automatically appended.
///
/// @note You MUST include `videoconvert` in your pipeline to convert to a supported format.
///       The backend will set caps filter on appsink to only accept BGR/RGB/BGRA/RGBA/GRAY8.
///
/// @example Valid pipelines:
///   - "videotestsrc ! videoconvert ! video/x-raw,format=BGR ! appsink name=sink"
///   - "filesrc location=video.mp4 ! decodebin ! videoconvert ! video/x-raw,format=BGR ! appsink name=sink"
///   - "v4l2src device=/dev/video0 ! videoconvert ! video/x-raw,format=BGR ! appsink name=sink"
///   - "rtspsrc location=rtsp://... ! decodebin ! videoconvert ! video/x-raw,format=RGB ! appsink name=sink"
///
/// @sa https://github.com/opencv/opencv/blob/4.x/modules/videoio/src/cap_gstreamer.cpp
struct GStreamerBackend {
	std::unique_ptr<GStreamerBackendImpl> impl;

	/// @brief Construct a GStreamer backend with a pipeline string
	/// @param pipeline The gst-launch-1.0 style pipeline string
	/// @param video_config Video configuration including looping behavior
	explicit GStreamerBackend(std::string pipeline, const app::VideoConfig &video_config);
	~GStreamerBackend();

	// Non-copyable, movable
	GStreamerBackend(const GStreamerBackend &)            = delete;
	GStreamerBackend &operator=(const GStreamerBackend &) = delete;
	GStreamerBackend(GStreamerBackend &&) noexcept;
	GStreamerBackend &operator=(GStreamerBackend &&) noexcept;

	void Init();
	void Shutdown();
	void SetOnMetadata(on_metadata_fn_t on_metadata);
	void SetOnFrame(on_frame_fn_t on_frame);
	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking);
	void SetOnError(on_error_fn_t on_error);
	source_info_t GetSourceInfo();
	std::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns);
	/**
	 * @brief Reset frame count to zero
	 * @return 0 on success, -EIO on I/O error
	 * @note For finite sources, this seeks back to the beginning unless
	 * `use_finite_as_infinite_stream` is true, when it only resets the internal
	 * frame count without any seeking.
	 */
	error_t ResetFrameCount();
	std::expected<recording_status_t, error_t> StartRecording(std::string_view output_path);
	std::expected<recording_status_t, error_t> StopRecording();
	std::expected<recording_status_t, error_t> GetRecordingStatus();
};

} // namespace app::backends

#endif /* EE8D58F7_CFE6_44F9_8A1A_BAF66882A291 */
