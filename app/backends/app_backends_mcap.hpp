#ifndef D3E7BA6F_0F90_4B1D_9BB4_7BCA7438468F
#define D3E7BA6F_0F90_4B1D_9BB4_7BCA7438468F

#include <memory>

#include "app_backends_facade.hpp"

namespace app {
struct McapConfig;
struct VideoConfig;
}

namespace app::backends {

struct McapBackendImpl;
struct McapBackend {
	std::unique_ptr<McapBackendImpl> impl;

	McapBackend(app::McapConfig mcap_config, const app::VideoConfig &video_config);
	~McapBackend();

	void Init();
	void Shutdown();
	void SetOnMetadata(on_metadata_fn_t on_metadata);
	void SetOnFrame(on_frame_fn_t on_frame);
	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking);
	void SetOnError(on_error_fn_t on_error);
	source_info_t GetSourceInfo();
	std::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns);
	error_t ResetFrameCount();
	std::expected<recording_status_t, error_t> StartRecording(std::string_view output_path);
	std::expected<recording_status_t, error_t> StopRecording();
	std::expected<recording_status_t, error_t> GetRecordingStatus();
	std::string GetLastRecordingError();
};

} // namespace app::backends

#endif /* D3E7BA6F_0F90_4B1D_9BB4_7BCA7438468F */
