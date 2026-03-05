#pragma once
#include <string>
#include <variant>
#include <optional>
#include <array>
#include <vector>
#include <app_enum_models.hpp>
#include <filesystem>

namespace app {

/// Backend type for video capture
enum class BackendType {
	OpenCV,
	GStreamer,
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

struct VideoConfig {
	/// backend type
	BackendType backend{BackendType::OpenCV};
	/// treat finite source as infinite stream (loop automatically, disable seeking)
	bool use_finite_as_infinite_stream{false};
	FiniteStreamEndingBehavior finite_stream_ending_behavior{FiniteStreamEndingBehavior::Stop};
};

enum class UndistortModel {
	Pinhole,
};

struct UndistortConfig {
	bool enabled{false};
	UndistortModel model{UndistortModel::Pinhole};
	std::array<double, 9> camera_matrix{1.0, 0.0, 0.0,
										0.0, 1.0, 0.0,
										0.0, 0.0, 1.0};
	std::vector<double> dist_coeffs;
	bool use_optimal_new_camera_matrix{true};
	double alpha{0.0};
	bool crop_to_valid_roi{false};
	bool strict_startup{false};
};

struct PreprocessConfig {
	std::optional<UndistortConfig> undistort;
};

struct Config {
	/// name of cvmmap server instance
	std::string name;
	/// video capture configuration
	VideoConfig video;
	/// OpenCV-specific config (used when backend == OpenCV)
	std::optional<OpenCVConfig> opencv;
	/// GStreamer-specific config (used when backend == GStreamer)
	std::optional<GStreamerConfig> gstreamer;
	std::optional<PreprocessConfig> preprocess;

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

	[[nodiscard]]
	std::string zmq_control_address() const {
		return "ipc:///tmp/" + shm_name() + "_control";
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

std::string_view to_string(UndistortModel model);
UndistortModel undistort_model_from_string(std::string_view s);

}
