#include <optional>
#include <utility>

#include "app_config.hpp"
#include "app_backends_playlist.hpp"

namespace {

using app::BackendType;
using app::Config;
using app::FilePlaylistConfig;
using app::McapConfig;
using app::ZedConfig;
using app::backends::MakePlaylistController;
using app::backends::ResolvedPlaylistState;

bool expect(bool condition) {
	return condition;
}

Config make_mcap_playlist_config() {
	Config config{};
	config.video.backend = BackendType::MCAP;
	config.mcap = McapConfig{
		.path = {},
		.video_topic = "/camera/video",
		.depth_topic = "/camera/depth",
		.body_topic = "/camera/body",
		.timestamp_domain = cvmmap::TimestampDomain::UnixEpochNs,
		.playlist = FilePlaylistConfig{
			.paths = {"first.mcap", "second.mcap"},
			.sort_by_recording_time = false,
		},
	};
	return config;
}

Config make_zed_playlist_config() {
	Config config{};
	config.video.backend = BackendType::ZED;
	ZedConfig zed{};
	zed.stream_mode = "svo";
	zed.playlist = FilePlaylistConfig{
		.paths = {"first.svo", "second.svo"},
		.sort_by_recording_time = false,
	};
	config.zed = std::move(zed);
	return config;
}

bool test_mcap_controller_tracks_state_and_config_path() {
	auto config = make_mcap_playlist_config();
	auto controller = MakePlaylistController(config);

	const auto configured = controller->GetConfiguredRequest();
	if (!expect(configured.has_value())) {
		return false;
	}
	if (!expect(configured->paths.size() == 2 && configured->paths[0] == "first.mcap")) {
		return false;
	}
	if (!expect(controller->SupportsRuntimeApply())) {
		return false;
	}
	if (!expect(!controller->HasPlaylist())) {
		return false;
	}

	controller->ReplaceState(ResolvedPlaylistState{
		.paths = {"first.mcap", "second.mcap"},
		.sort_by_recording_time = false,
		.current_index = 0,
	});

	if (!expect(controller->HasPlaylist())) {
		return false;
	}
	if (!expect(controller->Size() == 2 && controller->CurrentIndex() == 0)) {
		return false;
	}
	if (!expect(controller->CurrentPath().has_value() && *controller->CurrentPath() == "first.mcap")) {
		return false;
	}
	if (!expect(controller->SetCurrentIndex(1))) {
		return false;
	}
	if (!expect(!controller->SetCurrentIndex(2))) {
		return false;
	}

	controller->ApplyCurrentPathToConfig();
	if (!expect(config.mcap.has_value() && config.mcap->path == "second.mcap")) {
		return false;
	}

	const auto info = controller->GetInfo();
	if (!expect(info.has_playlist)) {
		return false;
	}
	if (!expect(info.paths.size() == 2 && info.current_index == 1 && info.current_path == "second.mcap")) {
		return false;
	}

	const auto snapshot = controller->SnapshotState();
	if (!expect(snapshot.has_value())) {
		return false;
	}
	controller->RestoreState(std::nullopt);
	if (!expect(!controller->HasPlaylist())) {
		return false;
	}
	controller->RestoreState(snapshot);
	return expect(controller->CurrentIndex() == 1);
}

bool test_zed_controller_tracks_state_and_config_path() {
	auto config = make_zed_playlist_config();
	auto controller = MakePlaylistController(config);

	const auto configured = controller->GetConfiguredRequest();
	if (!expect(configured.has_value() && configured->paths.size() == 2)) {
		return false;
	}
	if (!expect(controller->SupportsRuntimeApply())) {
		return false;
	}

	controller->ReplaceState(ResolvedPlaylistState{
		.paths = {"first.svo", "second.svo"},
		.sort_by_recording_time = false,
		.current_index = 1,
	});
	controller->ApplyCurrentPathToConfig();

	if (!expect(config.zed.has_value())) {
		return false;
	}
	if (!expect(config.zed->svo_path.has_value() && *config.zed->svo_path == "second.svo")) {
		return false;
	}

	auto local_mode = config;
	local_mode.zed->stream_mode = "local";
	auto local_controller = MakePlaylistController(local_mode);
	if (!expect(!local_controller->GetConfiguredRequest().has_value())) {
		return false;
	}
	return expect(!local_controller->SupportsRuntimeApply());
}

bool test_resolve_rejects_invalid_and_unsupported_requests() {
	Config dummy_config{};
	dummy_config.video.backend = BackendType::Dummy;
	auto controller = MakePlaylistController(dummy_config);

	auto empty_request = controller->Resolve(cvmmap::PlaylistRequest{});
	if (!expect(!empty_request)) {
		return false;
	}
	if (!expect(empty_request.error().code == cvmmap::ControlErrorCode::InvalidPayload)) {
		return false;
	}

	auto empty_path_request = controller->Resolve(cvmmap::PlaylistRequest{
		.paths = {"good", ""},
		.sort_by_recording_time = false,
	});
	if (!expect(!empty_path_request)) {
		return false;
	}
	if (!expect(empty_path_request.error().code == cvmmap::ControlErrorCode::InvalidPayload)) {
		return false;
	}

	auto unsupported_request = controller->Resolve(cvmmap::PlaylistRequest{
		.paths = {"only-item"},
		.sort_by_recording_time = false,
	});
	if (!expect(!unsupported_request)) {
		return false;
	}
	return expect(
		unsupported_request.error().message == "playlist apply is only supported for MCAP and ZED SVO producers");
}

bool test_resolve_matches_backend_build_gates() {
	{
		auto config = make_mcap_playlist_config();
		auto controller = MakePlaylistController(config);
		auto resolved = controller->Resolve(cvmmap::PlaylistRequest{
			.paths = {"first.mcap", "second.mcap"},
			.sort_by_recording_time = false,
		});
#ifdef WITH_BACKEND_MCAP
		if (!expect(resolved.has_value())) {
			return false;
		}
		if (!expect(resolved->paths.size() == 2 && resolved->current_index == 0)) {
			return false;
		}
#else
		if (!expect(!resolved)) {
			return false;
		}
		if (!expect(resolved.error().message == "MCAP playlist apply is unavailable in this build")) {
			return false;
		}
#endif
	}

	{
		auto config = make_zed_playlist_config();
		auto controller = MakePlaylistController(config);
		auto resolved = controller->Resolve(cvmmap::PlaylistRequest{
			.paths = {"first.svo", "second.svo"},
			.sort_by_recording_time = false,
		});
#ifdef WITH_BACKEND_ZED
		if (!expect(resolved.has_value())) {
			return false;
		}
		if (!expect(resolved->paths.size() == 2 && resolved->current_index == 0)) {
			return false;
		}
#else
		if (!expect(!resolved)) {
			return false;
		}
		if (!expect(resolved.error().message == "ZED playlist apply is unavailable in this build")) {
			return false;
		}
#endif
	}

	{
		auto config = make_zed_playlist_config();
		config.zed->stream_mode = "local";
		auto controller = MakePlaylistController(config);
		auto resolved = controller->Resolve(cvmmap::PlaylistRequest{
			.paths = {"first.svo"},
			.sort_by_recording_time = false,
		});
		if (!expect(!resolved)) {
			return false;
		}
		if (!expect(resolved.error().message == "ZED playlist apply is only supported for stream_mode='svo'")) {
			return false;
		}
	}

	return true;
}

} // namespace

int main() {
	if (!test_mcap_controller_tracks_state_and_config_path()) {
		return 10;
	}
	if (!test_zed_controller_tracks_state_and_config_path()) {
		return 20;
	}
	if (!test_resolve_rejects_invalid_and_unsupported_requests()) {
		return 30;
	}
	if (!test_resolve_matches_backend_build_gates()) {
		return 40;
	}
	return 0;
}
