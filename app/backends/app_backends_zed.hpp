#ifndef CE5657DE_F3BD_4D12_B529_2DB6C5F4E72B
#define CE5657DE_F3BD_4D12_B529_2DB6C5F4E72B

#include <memory>
#include <cvmmap/compat/expected.hpp>
#include "app_backends_facade.hpp"

namespace app {
struct VideoConfig;
struct ZedConfig;
}

namespace app::backends {

struct ZedBackendImpl;

struct ZedBackend {
	std::unique_ptr<ZedBackendImpl> impl;

	ZedBackend(const app::ZedConfig &zed_config, const app::VideoConfig &video_config);
	~ZedBackend();

	void Init();
	void Shutdown();
	void SetOnMetadata(on_metadata_fn_t on_metadata);
	void SetOnFrame(on_frame_fn_t on_frame);
	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking);
	void SetOnError(on_error_fn_t on_error);
	source_info_t GetSourceInfo();
	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns);
	error_t ResetFrameCount();
	cvmmap::expected<recording_status_t, error_t> StartRecording(const svo_recording_request_t &request);
	cvmmap::expected<recording_status_t, error_t> StopRecording();
	cvmmap::expected<recording_status_t, error_t> GetRecordingStatus();
	std::string GetLastRecordingError();
};

cvmmap::expected<uint64_t, std::string> ProbeZedSvoStartTimestampNs(const app::ZedConfig &zed_config);

}

#endif
