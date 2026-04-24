#ifndef D3E7BA6F_0F90_4B1D_9BB4_7BCA7438468F
#define D3E7BA6F_0F90_4B1D_9BB4_7BCA7438468F

#include <memory>

#include <cvmmap/compat/expected.hpp>

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
	error_t ResetFrameCount();
};

cvmmap::expected<uint64_t, std::string> ProbeMcapStartTimestampNs(const app::McapConfig &mcap_config);

} // namespace app::backends

#endif /* D3E7BA6F_0F90_4B1D_9BB4_7BCA7438468F */
