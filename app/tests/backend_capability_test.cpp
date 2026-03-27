#include <cstdint>
#include <utility>

#include "app_backends_handle.hpp"
#include "app_config.hpp"

namespace {

using app::BackendType;
using app::DummyConfig;
using app::VideoConfig;
using app::backends::BackendHandle;
using app::backends::DummyBackend;
using app::backends::on_body_tracking_fn_t;
using app::backends::svo_recording_request_t;

template <typename Backend>
concept HasBodyTrackingSetter = requires(Backend &backend, on_body_tracking_fn_t callback) {
	backend.SetOnBodyTracking(std::move(callback));
};

template <typename Backend>
concept HasSvoRecordingMethods = requires(Backend &backend, const svo_recording_request_t &request) {
	backend.StartRecording(request);
	backend.StopRecording();
	backend.GetRecordingStatus();
	backend.GetLastRecordingError();
};

static_assert(!HasBodyTrackingSetter<DummyBackend>);
static_assert(!HasSvoRecordingMethods<DummyBackend>);

#ifdef WITH_BACKEND_OPENCV
static_assert(!HasBodyTrackingSetter<app::backends::OpenCVBackend>);
static_assert(!HasSvoRecordingMethods<app::backends::OpenCVBackend>);
#endif

#ifdef WITH_BACKEND_GSTREAMER
static_assert(!HasBodyTrackingSetter<app::backends::GStreamerBackend>);
static_assert(!HasBodyTrackingSetter<app::backends::UdpRtpBackend>);
static_assert(!HasSvoRecordingMethods<app::backends::GStreamerBackend>);
static_assert(!HasSvoRecordingMethods<app::backends::UdpRtpBackend>);
#endif

#ifdef WITH_BACKEND_MCAP
static_assert(HasBodyTrackingSetter<app::backends::McapBackend>);
static_assert(!HasSvoRecordingMethods<app::backends::McapBackend>);
#endif

#ifdef WITH_BACKEND_ZED
static_assert(HasBodyTrackingSetter<app::backends::ZedBackend>);
static_assert(HasSvoRecordingMethods<app::backends::ZedBackend>);
#endif

int test_backend_handle_rejects_body_tracking_for_dummy() {
	DummyConfig dummy_cfg{};
	VideoConfig video_cfg{};
	video_cfg.backend = BackendType::Dummy;

	BackendHandle backend;
	backend.emplace<DummyBackend>(dummy_cfg, video_cfg);

	bool registered = false;
	try {
		registered = backend.TrySetOnBodyTracking([](const cvmmap::body_tracking_frame_t &) {});
	} catch (...) {
		return 2;
	}
	return registered ? 1 : 0;
}

int test_backend_handle_rejects_svo_recording_for_dummy() {
	DummyConfig dummy_cfg{};
	VideoConfig video_cfg{};
	video_cfg.backend = BackendType::Dummy;

	BackendHandle backend;
	backend.emplace<DummyBackend>(dummy_cfg, video_cfg);

	bool visited   = false;
	bool supported = false;
	try {
		supported = backend.TryVisitSvoRecordable([&](auto &) {
			visited = true;
		});
	} catch (...) {
		return 2;
	}
	if (supported) {
		return 1;
	}
	return visited ? 3 : 0;
}

} // namespace

int main() {
	if (const auto rc = test_backend_handle_rejects_body_tracking_for_dummy(); rc != 0) {
		return 10 + rc;
	}
	if (const auto rc = test_backend_handle_rejects_svo_recording_for_dummy(); rc != 0) {
		return 20 + rc;
	}
	return 0;
}
