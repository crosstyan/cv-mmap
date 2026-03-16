#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <cvmmap/ipc.hpp>
#include <cvmmap/backend_types.hpp>

namespace cvmmap {

struct NatsControlHandlers {
	std::function<int()> on_reset_frame_count;
	std::function<app::backends::source_info_t()> on_get_source_info;
	std::function<bool()> on_source_can_seek;
	std::function<std::expected<app::backends::seek_result_t, int>(uint64_t)> on_seek_timestamp;
	std::function<bool()> on_svo_recording_available;
	std::function<std::expected<app::backends::recording_status_t, int>(const app::backends::svo_recording_request_t &)> on_start_svo_recording;
	std::function<std::expected<app::backends::recording_status_t, int>()> on_stop_svo_recording;
	std::function<std::expected<app::backends::recording_status_t, int>()> on_get_svo_recording_status;
	std::function<std::string()> on_get_svo_last_recording_error;
};

class NatsControlService {
public:
	NatsControlService(std::string instance_name, std::string target_key, std::string nats_url);
	~NatsControlService();

	NatsControlService(const NatsControlService &) = delete;
	NatsControlService &operator=(const NatsControlService &) = delete;

	void SetHandlers(NatsControlHandlers handlers);
	void Start();
	void Stop();

	void PublishModuleStatus(int32_t status_code);
	void PublishBodyTracking(std::span<const uint8_t> raw_bytes);

private:
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

} // namespace cvmmap
