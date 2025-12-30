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

struct Config {
	/// name of cvmmap server instance
	std::string name;
	/// backend type: opencv or gstreamer
	BackendType backend = BackendType::OpenCV;
	/// treat finite source as infinite stream (loop automatically, disable seeking)
	bool use_finite_as_infinite_stream = false;
	/// OpenCV-specific config (used when backend == OpenCV)
	std::optional<OpenCVConfig> opencv;
	/// GStreamer-specific config (used when backend == GStreamer)
	std::optional<GStreamerConfig> gstreamer;

	static Config Default();

	static Config from_toml(const std::filesystem::path &path);

	[[nodiscard]]
	std::string to_toml() const;

	[[nodiscard]]
	std::string shm_name() const {
		return "cvmmap_" + name;
	}

	[[nodiscard]]
	std::string zmq_address() const {
		return "ipc:///tmp/" + shm_name();
	}
};

/// Convert BackendType to string
std::string_view to_string(BackendType backend);
/// Parse BackendType from string
BackendType backend_from_string(std::string_view s);

}