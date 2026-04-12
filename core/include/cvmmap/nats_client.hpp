#pragma once

#include <chrono>
#include <cstdint>
#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/functional.hpp>
#include <memory>
#include <span>
#include <string>

#include <cvmmap/ipc.hpp>
#include <cvmmap/client.hpp>

namespace cvmmap {

class NatsControlClient {
public:
	NatsControlClient(std::string target_key, std::string nats_url);
	~NatsControlClient();

	NatsControlClient(const NatsControlClient &) = delete;
	NatsControlClient &operator=(const NatsControlClient &) = delete;

	[[nodiscard]]
	bool Start();
	void Stop();

	// Control (NATS request-reply, blocking with timeout)
	ControlErrorCode
	ResetFrameCount(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<SourceInfo, ControlErrorCode>
	GetSourceInfo(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<SeekResult, ControlErrorCode>
	SeekTimestampNs(uint64_t ts, std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<PlaylistInfo, ControlError>
	ApplyPlaylist(const PlaylistRequest &request,
				  std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<PlaylistInfo, ControlError>
	GetPlaylistInfo(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<SourceControlCapabilities, ControlError>
	GetSourceCapabilities(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<SvoRecordingCapabilities, ControlError>
	GetSvoRecordingCapabilities(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<SvoRecordingStatus, ControlError>
	StartSvoRecording(const SvoRecordingRequest &request,
					  std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<SvoRecordingStatus, ControlError>
	StopSvoRecording(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	cvmmap::expected<SvoRecordingStatus, ControlError>
	GetSvoRecordingStatus(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

	// Subscriptions (NATS pub/sub)
	using OnBodyTrackingCallback = cvmmap::move_only_function<void(const body_tracking_frame_t &)>;
	using OnBodyTrackingRawCallback = cvmmap::move_only_function<void(std::span<const uint8_t>)>;
	using OnModuleStatusCallback = cvmmap::move_only_function<void(ModuleStatus status)>;
	void SetBodyTrackingCallback(OnBodyTrackingCallback &&cb);
	void SetBodyTrackingRawCallback(OnBodyTrackingRawCallback &&cb);
	void SetModuleStatusCallback(OnModuleStatusCallback &&cb);

private:
	struct impl;
	std::unique_ptr<impl> pimpl_;
};

} // namespace cvmmap
