#ifndef D85CE6BB_6714_4CC0_871D_EB7629F6E811
#define D85CE6BB_6714_4CC0_871D_EB7629F6E811
#include <memory>
#include <variant>
#include "app_backends_facade.hpp"
#include "app_enum_models.hpp"

namespace app::backends {
using opencv_parameter_t = std::variant<int, std::string>;

struct OpenCVBackendImpl;
struct OpenCVBackend {
	std::unique_ptr<OpenCVBackendImpl> impl;

	OpenCVBackend(opencv_parameter_t parameter,
				  bool looping                         = false,
				  app::VideoCaptureAPIs api_preference = app::VideoCaptureAPIs::CAP_ANY);
	~OpenCVBackend();

	void Init();
	void Shutdown();
	void SetOnMetadata(on_metadata_fn_t on_metadata);
	void SetOnFrame(on_frame_fn_t on_frame);
	error_t SeekFrame(size_t frame_index);
};
}

#endif /* D85CE6BB_6714_4CC0_871D_EB7629F6E811 */
