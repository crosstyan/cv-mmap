#ifndef B64F0D43_7776_4423_8B0F_8C67333E4434
#define B64F0D43_7776_4423_8B0F_8C67333E4434

#include <cstdint>
#include <memory>
#include <vector>

#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/functional.hpp>
#include <cvmmap/ipc.hpp>

#include "app_backends_facade.hpp"

namespace app {
struct UdpRtpConfig;
struct VideoConfig;
}

namespace app::backends {

struct encoded_access_unit_t {
	cvmmap::EncodedCodec codec{cvmmap::EncodedCodec::Unknown};
	cvmmap::EncodedBitstreamFormat bitstream_format{cvmmap::EncodedBitstreamFormat::Unknown};
	uint16_t flags{0};
	uint16_t frame_rate_num{0};
	uint16_t frame_rate_den{0};
	uint64_t source_timestamp_ns{0};
	uint64_t stream_pts_ns{0};
	std::vector<uint8_t> bytes{};
};

using on_encoded_access_unit_fn_t =
	cvmmap::move_only_function<void(const encoded_access_unit_t &access_unit)>;

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
	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns);
	error_t ResetFrameCount();
};

} // namespace app::backends

#endif /* B64F0D43_7776_4423_8B0F_8C67333E4434 */
