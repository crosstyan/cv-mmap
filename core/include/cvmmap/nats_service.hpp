#pragma once

#include <cstdint>
#include <cvmmap/compat/expected.hpp>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <cvmmap/client.hpp>
#include <cvmmap/ipc.hpp>
#include <cvmmap/backend_types.hpp>

namespace cvmmap {

struct NatsControlHandlers {
	std::function<ControlErrorCode()> on_reset_frame_count;
	std::function<app::backends::source_info_t()> on_get_source_info;
	std::function<cvmmap::expected<app::backends::seek_result_t, ControlErrorCode>(uint64_t)> on_seek_timestamp;
	std::function<cvmmap::expected<PlaylistInfo, ControlError>(const PlaylistRequest &)> on_apply_playlist;
	std::function<cvmmap::expected<PlaylistInfo, ControlError>()> on_get_playlist_info;
	std::function<SvoRecordingCapabilities()> on_get_svo_recording_capabilities;
	std::function<cvmmap::expected<SvoRecordingStatus, ControlError>(const SvoRecordingRequest &)> on_start_svo_recording;
	std::function<cvmmap::expected<SvoRecordingStatus, ControlError>()> on_stop_svo_recording;
	std::function<cvmmap::expected<SvoRecordingStatus, ControlError>()> on_get_svo_recording_status;
};

struct NatsControlServiceOptions {
	std::string instance_name{};
	std::string namespace_name{};
	std::string ipc_prefix{};
	std::string base_name{};
	std::string target_key{};
	std::string shm_name{};
	std::string zmq_addr{};
	std::string backend{};
	std::string nats_url{};
	std::string build_revision{};
	std::string build_tag{};
	std::string build_branch{};
	std::string build_timestamp_utc{};
};

class NatsControlService {
public:
	explicit NatsControlService(NatsControlServiceOptions options);
	~NatsControlService();

	NatsControlService(const NatsControlService &) = delete;
	NatsControlService &operator=(const NatsControlService &) = delete;

	void SetHandlers(NatsControlHandlers handlers);
	[[nodiscard]]
	bool Start();
	void Stop();

	void PublishModuleStatus(ModuleStatus status);
	void PublishBodyTracking(std::span<const uint8_t> raw_bytes);

private:
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

} // namespace cvmmap
