#ifndef CE5657DE_F3BD_4D12_B529_2DB6C5F4E72B
#define CE5657DE_F3BD_4D12_B529_2DB6C5F4E72B

#include <memory>
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
	error_t SeekFrame(size_t frame_index);
	error_t ResetFrameCount();
};

}

#endif
