#ifndef D85CE6BB_6714_4CC0_871D_EB7629F6E811
#define D85CE6BB_6714_4CC0_871D_EB7629F6E811
#include <memory>
#include <variant>
#include <cvmmap/compat/expected.hpp>
#include "app_backends_facade.hpp"
#include "app_enum_models.hpp"

namespace app {
struct VideoConfig;
}

namespace app::backends {
using opencv_parameter_t = std::variant<std::string, int>;

struct OpenCVBackendImpl;
struct OpenCVBackend {
	std::unique_ptr<OpenCVBackendImpl> impl;

	OpenCVBackend(opencv_parameter_t parameter,
				  const app::VideoConfig &video_config,
				  app::VideoCaptureAPIs api_preference = app::VideoCaptureAPIs::CAP_ANY);
	~OpenCVBackend();

	void Init();
	void Shutdown();
	void SetOnMetadata(on_metadata_fn_t on_metadata);
	void SetOnFrame(on_frame_fn_t on_frame);
	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking);
	void SetOnError(on_error_fn_t on_error);
	source_info_t GetSourceInfo();
	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns);
	/**
	 * @brief Reset frame count to zero
	 * @return 0 on success, -EIO on I/O error
	 * @note For finite sources, this seeks back to the beginning unless
	 * `use_finite_as_infinite_stream` is true, when it only resets the internal
	 * frame count without any seeking.
	 */
	error_t ResetFrameCount();
	cvmmap::expected<recording_status_t, error_t> StartRecording(std::string_view output_path);
	cvmmap::expected<recording_status_t, error_t> StopRecording();
	cvmmap::expected<recording_status_t, error_t> GetRecordingStatus();
	std::string GetLastRecordingError();
};
}

#endif /* D85CE6BB_6714_4CC0_871D_EB7629F6E811 */
