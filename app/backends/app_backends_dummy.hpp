#ifndef D57681F4_C4D5_4B2D_9418_73A33CCB6011
#define D57681F4_C4D5_4B2D_9418_73A33CCB6011

#include <memory>

#include "app_backends_facade.hpp"

namespace app {
struct DummyConfig;
struct VideoConfig;
}

namespace app::backends {

struct DummyBackendImpl;
struct DummyBackend {
	std::unique_ptr<DummyBackendImpl> impl;

	DummyBackend(app::DummyConfig dummy_config, const app::VideoConfig &video_config);
	~DummyBackend();

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

#endif /* D57681F4_C4D5_4B2D_9418_73A33CCB6011 */
