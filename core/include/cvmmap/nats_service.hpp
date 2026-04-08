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
	std::function<int()> on_reset_frame_count;
	std::function<app::backends::source_info_t()> on_get_source_info;
	std::function<cvmmap::expected<app::backends::seek_result_t, int>(uint64_t)> on_seek_timestamp;
	std::function<cvmmap::expected<PlaylistInfo, ControlError>(const PlaylistRequest &)> on_apply_playlist;
	std::function<cvmmap::expected<PlaylistInfo, ControlError>()> on_get_playlist_info;
	std::function<bool(RecordingFormat)> on_recording_available;
	std::function<cvmmap::expected<RecordingStatus, ControlError>(const RecordingRequest &)> on_start_recording;
	std::function<cvmmap::expected<RecordingStatus, ControlError>(RecordingFormat)> on_stop_recording;
	std::function<cvmmap::expected<RecordingStatus, ControlError>(RecordingFormat)> on_get_recording_status;
};

class NatsControlService {
public:
	NatsControlService(std::string instance_name, std::string target_key, std::string nats_url);
	~NatsControlService();

	NatsControlService(const NatsControlService &) = delete;
	NatsControlService &operator=(const NatsControlService &) = delete;

	void SetHandlers(NatsControlHandlers handlers);
	[[nodiscard]]
	bool Start();
	void Stop();

	void PublishModuleStatus(int32_t status_code);
	void PublishBodyTracking(std::span<const uint8_t> raw_bytes);

private:
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

} // namespace cvmmap
