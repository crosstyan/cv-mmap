#ifndef EE8D58F7_CFE6_44F9_8A1A_BAF66882A291
#define EE8D58F7_CFE6_44F9_8A1A_BAF66882A291

#include <memory>

#include <cvmmap/compat/expected.hpp>

#include "app_backends_facade.hpp"

namespace app {
struct VideoConfig;
}

namespace app::backends {

struct GStreamerBackendImpl;

struct GStreamerBackend {
	std::unique_ptr<GStreamerBackendImpl> impl;

	explicit GStreamerBackend(std::string pipeline, const app::VideoConfig &video_config);
	~GStreamerBackend();

	GStreamerBackend(const GStreamerBackend &) = delete;
	GStreamerBackend &operator=(const GStreamerBackend &) = delete;
	GStreamerBackend(GStreamerBackend &&) noexcept;
	GStreamerBackend &operator=(GStreamerBackend &&) noexcept;

	void Init();
	void Shutdown();
	void SetOnMetadata(on_metadata_fn_t on_metadata);
	void SetOnFrame(on_frame_fn_t on_frame);
	void SetOnError(on_error_fn_t on_error);
	source_info_t GetSourceInfo();
	/**
	 * @brief Reset frame count to zero
	 * @return 0 on success, -EIO on I/O error
	 * @note For finite sources, this seeks back to the beginning for `stop` and
	 * `loop` modes. In `loop_silent` mode, it only resets the internal frame
	 * count without seeking.
	 */
	error_t ResetFrameCount();
};

} // namespace app::backends

#endif /* EE8D58F7_CFE6_44F9_8A1A_BAF66882A291 */
