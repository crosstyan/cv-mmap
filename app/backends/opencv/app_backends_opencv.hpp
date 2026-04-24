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

#endif /* D85CE6BB_6714_4CC0_871D_EB7629F6E811 */
