#pragma once
#include <string>
#include <variant>
#include <optional>
#include <array>
#include <vector>
#include <app_enum_models.hpp>
#include <filesystem>
#include <format>

#include <cvmmap/target.hpp>

namespace app {

/// Backend type for video capture
enum class BackendType {
	Dummy,
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

struct DummyConfig {
	int width{1280};
	int height{720};
	int fps{30};
	uint32_t frames{0};
	int startup_delay_ms{0};
};

struct ZedConfig {
	struct BodyTrackingConfig {
		bool enabled{false};
		std::string detection_model{"HUMAN_BODY_ACCURATE"};
		std::string body_format{"BODY_18"};
		std::string body_selection{"FULL"};
		bool enable_body_fitting{false};
		bool allow_reduced_precision_inference{false};
		float max_range{-1.0f};
		float prediction_timeout_s{0.2f};
		float detection_confidence_threshold{20.0f};
		int minimum_keypoints_threshold{0};
		float skeleton_smoothing{0.0f};
	};

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
	std::optional<BodyTrackingConfig> body_tracking;
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
	std::optional<DummyConfig> dummy;
	std::optional<PreprocessConfig> preprocess;
	std::optional<ZedConfig> zed;

	static Config Default();

	static Config from_toml(const std::filesystem::path &path);

	[[nodiscard]]
	std::string to_toml() const;

	[[nodiscard]]
	std::string shm_name() const {
		return cvmmap::resolve_cvmmap_target_or_throw(
			std::format("cvmmap://{}@{}?namespace={}", name, ipc.prefix, ipc.name_space))
			.shm_name;
	}

	[[nodiscard]]
	std::string zmq_address() const {
		return cvmmap::resolve_cvmmap_target_or_throw(
			std::format("cvmmap://{}@{}?namespace={}", name, ipc.prefix, ipc.name_space))
			.zmq_addr;
	}

	[[nodiscard]]
	std::string zmq_control_address() const {
		return cvmmap::resolve_cvmmap_target_or_throw(
			std::format("cvmmap://{}@{}?namespace={}", name, ipc.prefix, ipc.name_space))
			.zmq_control_addr;
	}

	[[nodiscard]]
	std::string zmq_body_address() const {
		return cvmmap::resolve_cvmmap_target_or_throw(
			std::format("cvmmap://{}@{}?namespace={}", name, ipc.prefix, ipc.name_space))
			.zmq_body_addr;
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
