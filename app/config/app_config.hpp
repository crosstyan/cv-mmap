#pragma once
#include <string>
#include <variant>
#include <optional>
#include <app_enum_models.hpp>
#include <filesystem>

namespace app {

/// Backend type for video capture
enum class BackendType {
	OpenCV,
	GStreamer,
	ZED,
};

enum class FiniteStreamEndingBehavior {
	Stop,
	Loop,
};

/// OpenCV-specific configuration
struct OpenCVConfig {
	/// pipeline string or device index
	std::variant<std::string, int> parameter;
	/// API preference used by OpenCV
	VideoCaptureAPIs api_preference = CAP_ANY;
};

/// GStreamer-specific configuration
struct GStreamerConfig {
	/// GStreamer pipeline string
	std::string pipeline;
};

struct ZedConfig {
	std::optional<int> serial;
	std::optional<int> index;
	std::string stream_mode{"local"};
	std::optional<std::string> ip_address;
	std::optional<int> port;
	std::string resolution;
	int fps{};
	std::string depth_mode;
	int open_timeout_ms{10000};
	int warmup_frames{15};
	int max_consecutive_failures{30};
	int reconnect_interval_ms{1000};
	bool reconnect{true};
	std::string left_pixel_format{"bgr8"};
};

struct VideoConfig {
	/// backend type
	BackendType backend{BackendType::OpenCV};
	/// treat finite source as infinite stream (loop automatically, disable seeking)
	bool use_finite_as_infinite_stream{false};
	FiniteStreamEndingBehavior finite_stream_ending_behavior{FiniteStreamEndingBehavior::Stop};
};

struct IpcConfig {
	std::string name_space{"cvmmap"};
	std::string prefix{"/tmp"};
};

struct Config {
	/// name of cvmmap server instance
	std::string name;
	/// video capture configuration
	VideoConfig video;
	IpcConfig ipc;
	/// OpenCV-specific config (used when backend == OpenCV)
	std::optional<OpenCVConfig> opencv;
	/// GStreamer-specific config (used when backend == GStreamer)
	std::optional<GStreamerConfig> gstreamer;
	std::optional<ZedConfig> zed;

	static Config Default();

	static Config from_toml(const std::filesystem::path &path);

	[[nodiscard]]
	std::string to_toml() const;

	[[nodiscard]]
	std::string shm_name() const {
		return ipc.name_space + "_" + name;
	}

	[[nodiscard]]
	std::string zmq_address() const {
		return "ipc://" + ipc.prefix + "/" + shm_name();
	}

	[[nodiscard]]
	std::string zmq_control_address() const {
		return "ipc://" + ipc.prefix + "/" + shm_name() + "_control";
	}
};

/// Convert BackendType to string
std::string_view to_string(BackendType backend);
/// Parse BackendType from string
BackendType backend_from_string(std::string_view s);

/// Convert FiniteStreamEndingBehavior to string
std::string_view to_string(FiniteStreamEndingBehavior behavior);
/// Parse FiniteStreamEndingBehavior from string
FiniteStreamEndingBehavior finite_stream_ending_behavior_from_string(std::string_view s);

}
