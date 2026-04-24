#ifndef B64F0D43_7776_4423_8B0F_8C67333E4434
#define B64F0D43_7776_4423_8B0F_8C67333E4434

#include <memory>

#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/functional.hpp>
#include <cvmmap/ipc.hpp>

#include "app_backends_facade.hpp"

namespace app {
struct UdpRtpConfig;
struct VideoConfig;
}

namespace app::backends {

struct UdpRtpBackendImpl;
struct UdpRtpBackend {
	std::unique_ptr<UdpRtpBackendImpl> impl;

	UdpRtpBackend(app::UdpRtpConfig config, const app::VideoConfig &video_config);
	~UdpRtpBackend();

	void Init();
	void Shutdown();
	void SetOnMetadata(on_metadata_fn_t on_metadata);
	void SetOnFrame(on_frame_fn_t on_frame);
	void SetOnError(on_error_fn_t on_error);
	void SetOnEncodedAccessUnit(on_encoded_access_unit_fn_t on_encoded_access_unit);
	source_info_t GetSourceInfo();
	error_t ResetFrameCount();
};

} // namespace app::backends

#endif /* B64F0D43_7776_4423_8B0F_8C67333E4434 */
