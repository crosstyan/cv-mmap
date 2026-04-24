#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cvmmap/compat/format.hpp>
#include <csignal>
#include <limits>
#include <optional>
#include <functional>
#include <string_view>
#include <string>
#include <sstream>
#include <unordered_map>
#include <cvmmap/compat/expected.hpp>
#include <mutex>
#include <thread>
#include <vector>
#include <span>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "version/app_version.hpp"
#include "config/app_config.hpp"
#include "models/app_metadata_models.hpp"
#include "app_utils.hpp"
#include "backend_runtime.hpp"
#include "backends/app_backend_factory.hpp"
#include "backends/app_backends_playlist.hpp"
#include "frame_publisher.hpp"
#include <cvmmap/nats_service.hpp>

#if defined(__APPLE__) && defined(__MACH__)
#define __APP_MACOS__
#endif
#ifdef __APP_MACOS__
// https://en.wikipedia.org/wiki/Unistd.h
#include <unistd.h>
#endif

// APP_DEBUG_SYNC_MESSAGE_DUMP

namespace {
enum class ProcessExitCode : int {
	Success = 0,
	Failure = 1,
	FatalCameraRecovery = 32,
};
} // namespace

int main(int argc, char **argv) {
	using namespace app;
	constexpr auto IPC_PREFIX = "ipc://";
	app::version::print_version();

	CLI::App app{"Video Stream mmap adapter"};
	argv = app.ensure_utf8(argv);
	// default config file name is config.toml in cwd
	static std::string config_file = "config.toml";
	app.add_option("-c,--config", config_file, "Config file path");
	static bool use_default = false;
	app.add_flag("--default", use_default, "Use default config");
	static bool use_debug = false;
	app.add_flag("-d,--debug", use_debug, "Enable debug log");
	static bool use_trace = false;
	app.add_flag("--trace", use_trace, "Enable trace log");
	CLI11_PARSE(app, argc, argv);
	if (use_trace) {
		spdlog::set_level(spdlog::level::trace);
	} else if (use_debug) {
		spdlog::set_level(spdlog::level::debug);
	} else {
		spdlog::set_level(spdlog::level::info);
	}

	const std::filesystem::path config_path = config_file;
	if (not std::filesystem::exists(config_path)) {
		if (use_default) {
			std::ofstream ofs(config_file);
			ofs << app::Config::Default().to_toml();
			ofs.close();
			spdlog ::info("Create default config file in `{}`; Please restart the program.", config_file);
			return 0;
		} else {
			spdlog::error("Config file not found in `{}`. Use --default to create a default config", config_file);
			return 1;
		}
	}

	app::Config config;
	try {
		config = app::Config::from_toml(config_path);
	} catch (const std::exception &e) {
		spdlog::error("loading config: {}", e.what());
		return 1;
	}

	const auto resolved_target = cvmmap::resolve_cvmmap_target_or_throw(
		cvmmap::format(
			"cvmmap://{}@{}?namespace={}",
			config.name,
			config.ipc.prefix,
			config.ipc.name_space));

	// https://libzmq.readthedocs.io/en/latest/zmq_ipc.html
	// https://libzmq.readthedocs.io/en/latest/zmq_inproc.html
	// note that `zmq::socket_t` is RAII aware already
	zmq::context_t ctx;
	zmq::socket_t sock(ctx, zmq::socket_type::pub);
	deferrer zmq_deferrer([&sock, &ctx, zmq_address = config.zmq_address()] {
		if (zmq_address.starts_with(IPC_PREFIX)) {
			const auto path = zmq_address.substr(std::string_view(IPC_PREFIX).size());
			const auto err  = unlink(path.c_str());
			if (err == -1) {
				spdlog::error("unlink ZMQ address `{}` because of `{} ({})`", path, strerror(errno), errno);
			}
		}
	});

	try {
		// https://zguide.zeromq.org/docs/chapter2/
		// The inter-process ipc transport is disconnected, like tcp. It has one
		// limitation: it does not yet work on Windows. By convention we use
		// endpoint names with an "extension to avoid potential conflict
		// with other file names. On UNIX systems, if you use ipc endpoints you
		// need to create these with appropriate permissions otherwise they may
		// not be shareable between processes running under different user IDs.
		// You must also make sure all processes can access the files, e.g., by
		// running in the same working directory.
		sock.bind(config.zmq_address());
		if (config.zmq_address().starts_with(IPC_PREFIX)) {
			const auto path = config.zmq_address().substr(std::string_view(IPC_PREFIX).size());
			// 777
			const auto ok = chmod(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
			if (ok == -1) {
				spdlog::warn("chmod ZMQ address `{}` because of `{}`", path, strerror(errno));
			}
		}
	} catch (const zmq::error_t &e) {
		spdlog::error("bind to ZMQ address: `{}`", e.what());
		return 1;
	}
	spdlog::info("bond to ZMQ address: `{}`", config.zmq_address());

	const bool nats_enabled = config.nats.enabled;
	std::unique_ptr<cvmmap::NatsControlService> nats_service;
	if (nats_enabled) {
		nats_service = std::make_unique<cvmmap::NatsControlService>(
			cvmmap::NatsControlServiceOptions{
				.instance_name = resolved_target.instance,
				.namespace_name = resolved_target.namespace_name,
				.ipc_prefix = resolved_target.prefix,
				.base_name = resolved_target.base_name,
				.target_key = resolved_target.nats_target_key,
				.shm_name = resolved_target.shm_name,
				.zmq_addr = resolved_target.zmq_addr,
				.backend = std::string(app::to_string(config.video.backend)),
				.nats_url = config.nats.url,
				.build_revision = std::string(app::version::revision()),
				.build_tag = std::string(app::version::tag()),
				.build_branch = std::string(app::version::branch()),
				.build_timestamp_utc =
					std::string(app::version::compile_timestamp_utc()),
			});
	} else {
		spdlog::warn("NATS disabled; control/status and body-tracking transport are unavailable");
	}

	static auto is_running   = std::atomic_bool{true};
	static auto sigint_count = std::atomic_int{0};
	int exit_code = static_cast<int>(ProcessExitCode::Success);

	/**
	 * @brief signal handler for SIGINT
	 */
	constexpr auto sigint_handler = [](int) {
		if (sigint_count.fetch_add(1, std::memory_order::relaxed) > 0) {
			spdlog::critical("SIGINT received twice, force killing...");
			std::exit(1);
		}
		spdlog::info("SIGINT received, stopping...");
		is_running.store(false, std::memory_order::relaxed);
	};
	std::signal(SIGINT, sigint_handler);

	auto frame_publisher = FramePublisher::Create(config, sock);
	if (!frame_publisher) {
		spdlog::error("{}", frame_publisher.error());
		return 1;
	}
	BodyTrackingPublisher body_tracking_publisher(config.name, nats_service.get());
	BackendRuntime runtime(config, *frame_publisher, body_tracking_publisher);

	const auto send_status = [&nats_service, nats_enabled](cvmmap::ModuleStatus status) {
		if (!nats_enabled || !nats_service) {
			return;
		}
		nats_service->PublishModuleStatus(status);
	};

	auto playlist_controller = app::backends::MakePlaylistController(config);
	if (auto configured_playlist = playlist_controller->GetConfiguredRequest();
		configured_playlist.has_value()) {
		auto resolved_playlist = playlist_controller->Resolve(*configured_playlist);
		if (!resolved_playlist) {
			spdlog::error(
				"configured playlist resolution is invalid: {}",
				resolved_playlist.error().message);
			return 1;
		}
		playlist_controller->ReplaceState(std::move(*resolved_playlist));
		spdlog::info(
			"configured {} playlist items for backend '{}'",
			playlist_controller->Size(),
			app::to_string(config.video.backend));
	}

	std::atomic<PlaylistTransitionAction> pending_playlist_transition{
		PlaylistTransitionAction::None};

	const auto request_playlist_transition = [&pending_playlist_transition](
		PlaylistTransitionAction action) {
		auto expected = PlaylistTransitionAction::None;
		(void)pending_playlist_transition.compare_exchange_strong(
			expected,
			action,
			std::memory_order_relaxed);
	};
	const auto request_playlist_item_transition =
		[&config, &playlist_controller, &request_playlist_transition](
			const bool require_alternative_item) {
			if (!playlist_controller->HasPlaylist()) {
				return false;
			}

			const auto size = playlist_controller->Size();
			const auto current_index = playlist_controller->CurrentIndex();
			if (current_index + 1 < size) {
				request_playlist_transition(PlaylistTransitionAction::Advance);
				return true;
			}
			if (require_alternative_item && size < 2) {
				return false;
			}
			if (config.video.finite_source_loops_silently()) {
				request_playlist_transition(PlaylistTransitionAction::RewindSilent);
				return true;
			}
			if (config.video.finite_source_loop_emits_reset()) {
				request_playlist_transition(PlaylistTransitionAction::RewindEmitReset);
				return true;
			}
			return false;
		};
	playlist_controller->ApplyCurrentPathToConfig();

	// Mutex to protect backend calls from concurrent NATS and ZMQ threads
	std::mutex backend_control_mutex;
	const BackendRuntimeCallbacks runtime_callbacks{
		.stop_running = [&is_running]() {
			is_running.store(false, std::memory_order::relaxed);
		},
		.mark_fatal_camera_recovery = [&exit_code]() {
			exit_code = static_cast<int>(ProcessExitCode::FatalCameraRecovery);
		},
		.request_playlist_item_transition = request_playlist_item_transition,
		.request_playlist_transition = request_playlist_transition,
	};

	const auto switch_playlist_item = [&](const size_t target_index, const bool emit_reset) -> int {
		if (!playlist_controller->HasPlaylist() || !playlist_controller->SetCurrentIndex(target_index)) {
			return -EINVAL;
		}

		runtime.ShutdownActiveBackend();
		playlist_controller->ApplyCurrentPathToConfig();
		runtime.ResetFrameState();

		if (!runtime.InitializeActiveBackend(runtime_callbacks)) {
			is_running.store(false, std::memory_order::relaxed);
			return -EIO;
		}
		if (emit_reset) {
			send_status(cvmmap::ModuleStatus::StreamReset);
		}
		return backends::ERR_OK;
	};

	const auto any_recording_active = [&runtime]()
		-> cvmmap::expected<bool, cvmmap::ControlError> {
		const auto &svo_recorder_provider = runtime.svo_recorder_provider();
		if (!svo_recorder_provider || !svo_recorder_provider->status) {
			return false;
		}
		auto status = svo_recorder_provider->status();
		if (!status) {
			return cvmmap::unexpected(status.error());
		}
		return status->is_recording;
	};

	const auto apply_resolved_playlist =
		[&runtime,
		 &runtime_callbacks,
		 &playlist_controller,
		 &pending_playlist_transition,
		 &send_status,
		 &is_running](
			app::backends::ResolvedPlaylistState resolved_playlist,
			const bool emit_reset)
			-> cvmmap::expected<cvmmap::PlaylistInfo, cvmmap::ControlError> {
		const auto previous_state = playlist_controller->SnapshotState();
		const auto previous_source_path = runtime.SnapshotSourcePath();
		pending_playlist_transition.exchange(
			PlaylistTransitionAction::None,
			std::memory_order_relaxed);

		runtime.ShutdownActiveBackend();
		playlist_controller->ReplaceState(std::move(resolved_playlist));
		playlist_controller->ApplyCurrentPathToConfig();
		runtime.ResetFrameState();

		if (!runtime.InitializeActiveBackend(runtime_callbacks)) {
			spdlog::error("applied playlist activation is invalid; attempting rollback");
			playlist_controller->RestoreState(previous_state);
			runtime.RestoreSourcePath(previous_source_path);
			runtime.ResetFrameState();
			if (!runtime.InitializeActiveBackend(runtime_callbacks)) {
				spdlog::critical("playlist apply rollback error; stopping producer");
				is_running.store(false, std::memory_order::relaxed);
				return cvmmap::unexpected(cvmmap::ControlError{
					.code = cvmmap::ControlErrorCode::Error,
					.message = "playlist apply rollback error after activation error",
				});
			}
			return cvmmap::unexpected(cvmmap::ControlError{
				.code = cvmmap::ControlErrorCode::Error,
				.message = "applied playlist could not be activated; previous source restored",
			});
		}

		if (emit_reset) {
			send_status(cvmmap::ModuleStatus::StreamReset);
		}
		return playlist_controller->GetInfo();
	};

	if (!runtime.InitializeActiveBackend(runtime_callbacks)) {
		return 1;
	}


	// Wire up NATS handlers and start service only when transport is enabled.
	if (nats_enabled) {
		cvmmap::NatsControlHandlers nats_handlers;
		nats_handlers.on_reset_frame_count =
			[&runtime,
			 &backend_control_mutex,
			 &playlist_controller,
			 &switch_playlist_item]() -> cvmmap::ControlErrorCode {
				const auto map_control_error_code = [](const int error_code) {
					switch (error_code) {
					case 0:
						return cvmmap::ControlErrorCode::Ok;
					case -EOPNOTSUPP:
						return cvmmap::ControlErrorCode::Unsupported;
					case -EINVAL:
						return cvmmap::ControlErrorCode::InvalidPayload;
					case -ERANGE:
						return cvmmap::ControlErrorCode::OutOfRange;
					default:
						return cvmmap::ControlErrorCode::Error;
					}
				};
				std::lock_guard lock(backend_control_mutex);
				if (playlist_controller->HasPlaylist() &&
					playlist_controller->CurrentIndex() != 0) {
					return map_control_error_code(switch_playlist_item(0, false));
				}
				return map_control_error_code(runtime.assembly().backend->ResetFrameCount());
			};
		nats_handlers.on_get_source_info = [&runtime]() {
			return runtime.assembly().backend->GetSourceInfo();
		};
		nats_handlers.on_apply_playlist =
			[&apply_resolved_playlist,
			 &any_recording_active,
			 &backend_control_mutex,
			 &playlist_controller](
				const cvmmap::PlaylistRequest &request)
			-> cvmmap::expected<cvmmap::PlaylistInfo, cvmmap::ControlError> {
				std::lock_guard lock(backend_control_mutex);
				if (!playlist_controller->SupportsRuntimeApply()) {
					return cvmmap::unexpected(cvmmap::ControlError{
						.code = cvmmap::ControlErrorCode::Unsupported,
						.message = "playlist apply is only supported for MCAP and ZED SVO producers",
					});
				}
				auto recording_active = any_recording_active();
				if (!recording_active) {
					return cvmmap::unexpected(recording_active.error());
				}
				if (*recording_active) {
					return cvmmap::unexpected(cvmmap::ControlError{
						.code = cvmmap::ControlErrorCode::Error,
						.message = "cannot apply a playlist while recording is active",
					});
				}
				auto resolved_playlist = playlist_controller->Resolve(request);
				if (!resolved_playlist) {
					return cvmmap::unexpected(resolved_playlist.error());
				}
				return apply_resolved_playlist(std::move(*resolved_playlist), true);
			};
		nats_handlers.on_get_playlist_info =
			[&backend_control_mutex, &playlist_controller]()
			-> cvmmap::expected<cvmmap::PlaylistInfo, cvmmap::ControlError> {
				std::lock_guard lock(backend_control_mutex);
				return playlist_controller->GetInfo();
			};
		nats_handlers.on_get_camera_control_capabilities =
			[&backend_control_mutex, &runtime]()
			-> cvmmap::expected<cvmmap::CameraControlCapabilities, cvmmap::ControlError> {
				std::lock_guard lock(backend_control_mutex);
				const auto &camera_control_provider = runtime.camera_control_provider();
				if (!camera_control_provider || !camera_control_provider->capabilities) {
					return cvmmap::CameraControlCapabilities{};
				}
				return camera_control_provider->capabilities();
			};
		nats_handlers.on_get_camera_control =
			[&backend_control_mutex, &runtime](const cvmmap::CameraControlSetting setting)
			-> cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError> {
				std::lock_guard lock(backend_control_mutex);
				const auto &camera_control_provider = runtime.camera_control_provider();
				if (!camera_control_provider || !camera_control_provider->get) {
					return cvmmap::unexpected(cvmmap::ControlError{
						.code = cvmmap::ControlErrorCode::Unsupported,
						.message = "camera control is not supported by the active producer",
					});
				}
				return camera_control_provider->get(setting);
			};
		nats_handlers.on_set_camera_control =
			[&backend_control_mutex, &runtime](const cvmmap::CameraControlRequest &request)
			-> cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError> {
				std::lock_guard lock(backend_control_mutex);
				const auto &camera_control_provider = runtime.camera_control_provider();
				if (!camera_control_provider || !camera_control_provider->set) {
					return cvmmap::unexpected(cvmmap::ControlError{
						.code = cvmmap::ControlErrorCode::Unsupported,
						.message = "camera control is not supported by the active producer",
					});
				}
				return camera_control_provider->set(request);
			};
		nats_handlers.on_set_camera_control_range =
			[&backend_control_mutex, &runtime](
				const cvmmap::CameraControlRangeRequest &request)
			-> cvmmap::expected<cvmmap::CameraControlState, cvmmap::ControlError> {
				std::lock_guard lock(backend_control_mutex);
				const auto &camera_control_provider = runtime.camera_control_provider();
				if (!camera_control_provider || !camera_control_provider->set_range) {
					return cvmmap::unexpected(cvmmap::ControlError{
						.code = cvmmap::ControlErrorCode::Unsupported,
						.message = "camera control is not supported by the active producer",
					});
				}
				return camera_control_provider->set_range(request);
			};


		nats_handlers.on_get_svo_recording_capabilities =
			[&backend_control_mutex, &runtime]() -> cvmmap::SvoRecordingCapabilities {
				std::lock_guard lock(backend_control_mutex);
				const auto &svo_recorder_provider = runtime.svo_recorder_provider();
				return cvmmap::SvoRecordingCapabilities{
					.can_record =
						svo_recorder_provider &&
						svo_recorder_provider->is_available &&
						svo_recorder_provider->is_available(),
				};
			};
		nats_handlers.on_start_svo_recording =
			[&backend_control_mutex, &runtime](const cvmmap::SvoRecordingRequest &request)
			-> cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError> {
			std::lock_guard lock(backend_control_mutex);
			const auto &svo_recorder_provider = runtime.svo_recorder_provider();
			if (!svo_recorder_provider || !svo_recorder_provider->start) {
				return cvmmap::unexpected(cvmmap::ControlError{
					.code    = cvmmap::ControlErrorCode::Unsupported,
					.message = "SVO recording is not supported by the active producer",
				});
			}
			return svo_recorder_provider->start(request);
		};
		nats_handlers.on_stop_svo_recording =
			[&backend_control_mutex, &runtime]()
			-> cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError> {
			std::lock_guard lock(backend_control_mutex);
			const auto &svo_recorder_provider = runtime.svo_recorder_provider();
			if (!svo_recorder_provider || !svo_recorder_provider->stop) {
				return cvmmap::unexpected(cvmmap::ControlError{
					.code    = cvmmap::ControlErrorCode::Unsupported,
					.message = "SVO recording is not supported by the active producer",
				});
			}
			return svo_recorder_provider->stop();
		};
		nats_handlers.on_get_svo_recording_status =
			[&backend_control_mutex, &runtime]()
			-> cvmmap::expected<cvmmap::SvoRecordingStatus, cvmmap::ControlError> {
			std::lock_guard lock(backend_control_mutex);
			const auto &svo_recorder_provider = runtime.svo_recorder_provider();
			if (!svo_recorder_provider || !svo_recorder_provider->status) {
				return cvmmap::unexpected(cvmmap::ControlError{
					.code    = cvmmap::ControlErrorCode::Unsupported,
					.message = "SVO recording is not supported by the active producer",
				});
			}
			return svo_recorder_provider->status();
		};
		nats_service->SetHandlers(std::move(nats_handlers));
		if (!nats_service->Start()) {
			spdlog::error("NATS control service could not start on '{}'", config.nats.url);
			runtime.ShutdownActiveBackend();
			return 1;
		}
	}

	send_status(cvmmap::ModuleStatus::Online);
	while (is_running.load(std::memory_order::relaxed)) {
		const auto transition =
			pending_playlist_transition.exchange(PlaylistTransitionAction::None, std::memory_order_relaxed);
		if (transition != PlaylistTransitionAction::None) {
			std::lock_guard lock(backend_control_mutex);
			int rc = backends::ERR_OK;
			switch (transition) {
			case PlaylistTransitionAction::Advance:
				rc = switch_playlist_item(
					playlist_controller->CurrentIndex() + 1,
					true);
				break;
			case PlaylistTransitionAction::RewindEmitReset:
				rc = switch_playlist_item(0, true);
				break;
			case PlaylistTransitionAction::RewindSilent:
				rc = switch_playlist_item(0, false);
				break;
			case PlaylistTransitionAction::ResetActiveEmitReset:
			case PlaylistTransitionAction::ResetActiveSilent:
				rc = runtime.assembly().backend->ResetFrameCount();
				if (rc == backends::ERR_OK &&
					transition == PlaylistTransitionAction::ResetActiveEmitReset) {
					send_status(cvmmap::ModuleStatus::StreamReset);
				}
				break;
			case PlaylistTransitionAction::None:
			default:
				break;
			}
			if (rc != backends::ERR_OK) {
				spdlog::error("playlist transition error: {}", rc);
				if (transition != PlaylistTransitionAction::ResetActiveSilent) {
					is_running.store(false, std::memory_order::relaxed);
				}
			}
			continue;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
	}

	runtime.ShutdownActiveBackend();
	send_status(cvmmap::ModuleStatus::Offline);
	if (nats_service) {
		nats_service->Stop();
	}

	if (exit_code == static_cast<int>(ProcessExitCode::Success)) {
		spdlog::info("normally exit");
	} else {
		spdlog::error("exiting with status {}", exit_code);
	}
	return exit_code;
}
