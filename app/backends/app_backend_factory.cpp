#include "app_backend_factory.hpp"

#include <memory>
#include <utility>

#include "app_backends_dummy.hpp"
#include "app_config.hpp"
#ifdef WITH_BACKEND_OPENCV
#include "app_backends_opencv.hpp"
#endif
#ifdef WITH_BACKEND_GSTREAMER
#include "app_backends_gst.hpp"
#include "app_backends_udp_rtp.hpp"
#endif
#ifdef WITH_BACKEND_MCAP
#include "app_backends_mcap.hpp"
#endif
#ifdef WITH_BACKEND_ZED
#include "app_backends_zed.hpp"
#endif

namespace app::backends {
namespace {

template <typename Facade, typename Backend>
std::optional<pro::proxy<Facade>> make_optional_capability_proxy(
	const std::shared_ptr<Backend> &backend) {
	if constexpr (pro::proxiable<std::shared_ptr<Backend>, Facade>) {
		return pro::proxy<Facade>{backend};
	} else {
		return std::nullopt;
	}
}

template <typename Backend>
BackendAssembly make_backend_assembly(std::shared_ptr<Backend> backend) {
	BackendAssembly assembly{};
	assembly.backend = pro::proxy<IBackend>{backend};
	assembly.body_tracking =
		make_optional_capability_proxy<IBodyTrackingBackend>(backend);
	assembly.camera_control =
		make_optional_capability_proxy<ICameraControlBackend>(backend);
	assembly.svo_recordable =
		make_optional_capability_proxy<ISvoRecordableBackend>(backend);
	assembly.direct_frame =
		make_optional_capability_proxy<IDirectFrameBackend>(backend);
	assembly.encoded_access_unit =
		make_optional_capability_proxy<IEncodedAccessUnitBackend>(backend);
	return assembly;
}

template <typename Backend, typename... Args>
BackendAssembly make_backend_assembly(Args &&...args) {
	return make_backend_assembly(
		std::make_shared<Backend>(std::forward<Args>(args)...));
}

} // namespace

cvmmap::expected<BackendAssembly, std::string>
MakeBackendAssembly(const app::Config &config) {
	switch (config.video.backend) {
	case app::BackendType::Dummy:
		if (!config.dummy) {
			return cvmmap::unexpected(
				"Dummy backend selected but [dummy] config section missing");
		}
		return make_backend_assembly<DummyBackend>(*config.dummy, config.video);
#ifdef WITH_BACKEND_OPENCV
	case app::BackendType::OpenCV:
		if (!config.opencv) {
			return cvmmap::unexpected(
				"OpenCV backend selected but [opencv] config section missing");
		}
		return make_backend_assembly<OpenCVBackend>(
			config.opencv->parameter,
			config.video,
			config.opencv->api_preference);
#else
	case app::BackendType::OpenCV:
		return cvmmap::unexpected(
			"OpenCV backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_OPENCV=ON");
#endif
#ifdef WITH_BACKEND_GSTREAMER
	case app::BackendType::GStreamer:
		if (!config.gstreamer) {
			return cvmmap::unexpected(
				"GStreamer backend selected but [gstreamer] config section missing");
		}
		return make_backend_assembly<GStreamerBackend>(
			config.gstreamer->pipeline,
			config.video);
	case app::BackendType::UdpRtp:
		if (!config.udp_rtp) {
			return cvmmap::unexpected(
				"UdpRtp backend selected but [udp_rtp] config section missing");
		}
		return make_backend_assembly<UdpRtpBackend>(
			*config.udp_rtp,
			config.video);
#else
	case app::BackendType::GStreamer:
		return cvmmap::unexpected(
			"GStreamer backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_GSTREAMER=ON");
	case app::BackendType::UdpRtp:
		return cvmmap::unexpected(
			"UdpRtp backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_GSTREAMER=ON");
#endif
#ifdef WITH_BACKEND_MCAP
	case app::BackendType::MCAP:
		if (!config.mcap) {
			return cvmmap::unexpected(
				"MCAP backend selected but [mcap] config section missing");
		}
		return make_backend_assembly<McapBackend>(*config.mcap, config.video);
#else
	case app::BackendType::MCAP:
		return cvmmap::unexpected(
			"MCAP backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_MCAP=ON");
#endif
#ifdef WITH_BACKEND_ZED
	case app::BackendType::ZED:
		if (!config.zed) {
			return cvmmap::unexpected(
				"ZED backend selected but [zed] config section missing");
		}
		return make_backend_assembly<ZedBackend>(*config.zed, config.video);
#else
	case app::BackendType::ZED:
		return cvmmap::unexpected(
			"ZED backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_ZED=ON");
#endif
	default:
		return cvmmap::unexpected(
			"selected backend is not available in this build");
	}
}

} // namespace app::backends
