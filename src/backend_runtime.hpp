#pragma once

#include <functional>
#include <optional>
#include <string>

#include <cvmmap/compat/expected.hpp>
#include <cvmmap/nats_service.hpp>

#include "backends/app_backend_factory.hpp"
#include "config/app_config.hpp"
#include "frame_publisher.hpp"

namespace app {

enum class PlaylistTransitionAction {
	None,
	Advance,
	RewindEmitReset,
	RewindSilent,
	ResetActiveEmitReset,
	ResetActiveSilent,
};

struct CameraControlProvider {
	std::function<cvmmap::expected<cvmmap::CameraControlCapabilities, cvmmap::ControlError>()> capabilities;
	std::function<cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError>(cvmmap::CameraControlSetting)> get;
	std::function<cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError>(const cvmmap::CameraControlRequest &)> set;
	std::function<cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError>(const cvmmap::CameraControlRangeRequest &)> set_range;
};

struct SvoRecorderProvider {
	std::function<bool()> is_available;
	std::function<cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError>(const cvmmap::SvoRecordingRequest &)> start;
	std::function<cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError>()> stop;
	std::function<cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError>()> status;
};


struct BackendRuntimeCallbacks {
	std::function<void()> stop_running;
	std::function<void()> mark_fatal_camera_recovery;
	std::function<bool(bool require_alternative_item)> request_playlist_item_transition;
	std::function<void(PlaylistTransitionAction)> request_playlist_transition;
};

class BackendRuntime {
public:
	BackendRuntime(
		Config &config,
		FramePublisher &frame_publisher,
		BodyTrackingPublisher &body_tracking_publisher);

	bool InitializeActiveBackend(BackendRuntimeCallbacks callbacks);
	void ShutdownActiveBackend();
	void ResetFrameState();

	[[nodiscard]] backends::BackendAssembly &assembly();
	[[nodiscard]] const backends::BackendAssembly &assembly() const;
	[[nodiscard]] const std::optional<CameraControlProvider> &camera_control_provider() const;
	[[nodiscard]] const std::optional<SvoRecorderProvider> &svo_recorder_provider() const;
	[[nodiscard]] ActiveBackendSourceSnapshot SnapshotSourcePath() const;
	void RestoreSourcePath(const ActiveBackendSourceSnapshot &snapshot);

private:
	void RefreshCameraControlProvider();
	void RefreshSvoRecorderProvider();
	void BindBackendCallbacks();

	Config *config_{};
	FramePublisher *frame_publisher_{};
	BodyTrackingPublisher *body_tracking_publisher_{};
	backends::BackendAssembly assembly_{};
	std::optional<CameraControlProvider> camera_control_provider_{};
	std::optional<SvoRecorderProvider> svo_recorder_provider_{};
	BackendRuntimeCallbacks callbacks_{};
};

} // namespace app
