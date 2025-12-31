#ifndef D85CE6BB_6714_4CC0_871D_EB7629F6E811
#define D85CE6BB_6714_4CC0_871D_EB7629F6E811
#include <memory>
#include <variant>
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
	/**
	 * @brief Seek to specific frame index (only for finite sources)
	 * @param frame_index The target frame index to seek to
	 * @return 0 on success, -EOPNOTSUPP if not supported, -EINVAL if out of range, -EIO on I/O error
	 * @note when `use_finite_as_infinite_stream` is true, seeking is disabled and will return -EOPNOTSUPP
	 */
	error_t SeekFrame(size_t frame_index);
	/**
	 * @brief Reset frame count to zero
	 * @return 0 on success, -EIO on I/O error
	 * @note For finite sources, this seeks back to the beginning unless
	 * `use_finite_as_infinite_stream` is true, when it only resets the internal
	 * frame count without any seeking.
	 */
	error_t ResetFrameCount();
};
}

#endif /* D85CE6BB_6714_4CC0_871D_EB7629F6E811 */
