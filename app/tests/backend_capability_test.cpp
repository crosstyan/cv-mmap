#include "app_backend_factory.hpp"
#include "app_config.hpp"

namespace {

[[nodiscard]]
int test_dummy_backend_has_no_capability_proxies() {
	app::Config cfg   = app::Config::Default();
	cfg.video.backend = app::BackendType::Dummy;

	auto assembly = app::backends::MakeBackendAssembly(cfg);
	if (!assembly.has_value()) {
		return 1;
	}

	const auto &backend_assembly = assembly.value();
	if (!backend_assembly.backend) {
		return 2;
	}

	if (backend_assembly.body_tracking.has_value()) {
		return 3;
	}
	if (backend_assembly.camera_control.has_value()) {
		return 4;
	}
	if (backend_assembly.svo_recordable.has_value()) {
		return 5;
	}
	if (backend_assembly.direct_frame.has_value()) {
		return 6;
	}
	if (backend_assembly.encoded_access_unit.has_value()) {
		return 7;
	}
	return 0;
}

[[nodiscard]]
int test_udp_rtp_backend_has_streaming_capabilities() {
#ifdef WITH_BACKEND_GSTREAMER
	app::Config cfg   = app::Config::Default();
	cfg.video.backend = app::BackendType::UdpRtp;
	cfg.udp_rtp       = app::UdpRtpConfig{};

	auto assembly = app::backends::MakeBackendAssembly(cfg);
	if (!assembly.has_value()) {
		return 1;
	}

	const auto &backend_assembly = assembly.value();
	if (!backend_assembly.backend) {
		return 2;
	}
	if (!backend_assembly.direct_frame.has_value()) {
		return 3;
	}
	if (!backend_assembly.encoded_access_unit.has_value()) {
		return 4;
	}
	return 0;
#else
	return 0;
#endif
}

} // namespace

int main() {
	if (const auto rc = test_dummy_backend_has_no_capability_proxies(); rc != 0) {
		return 10 + rc;
	}
	if (const auto rc = test_udp_rtp_backend_has_streaming_capabilities(); rc != 0) {
		return 20 + rc;
	}
	return 0;
}
