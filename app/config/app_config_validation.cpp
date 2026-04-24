#include "app_config_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

#include <cvmmap/compat/format.hpp>
#include <spdlog/spdlog.h>

namespace app::config_detail {

void validate_undistort_config(const app::UndistortConfig &cfg) {
	if (!std::isfinite(cfg.alpha) || cfg.alpha < 0.0 || cfg.alpha > 1.0) {
		throw std::invalid_argument("preprocess.undistort.alpha must be finite and in [0, 1]");
	}

	for (const auto value : cfg.camera_matrix) {
		if (!std::isfinite(value)) {
			throw std::invalid_argument("preprocess.undistort.camera_matrix contains non-finite value");
		}
	}

	if (cfg.camera_matrix[0] <= 0.0 || cfg.camera_matrix[4] <= 0.0) {
		throw std::invalid_argument("preprocess.undistort.camera_matrix requires fx>0 and fy>0");
	}

	constexpr std::array<size_t, 5> valid_coeff_lengths{4, 5, 8, 12, 14};
	if (cfg.dist_coeffs.empty()) {
		throw std::invalid_argument("preprocess.undistort.dist_coeffs is required when undistort is enabled");
	}
	if (std::find(valid_coeff_lengths.begin(), valid_coeff_lengths.end(), cfg.dist_coeffs.size()) == valid_coeff_lengths.end()) {
		throw std::invalid_argument("preprocess.undistort.dist_coeffs length must be one of 4, 5, 8, 12, 14");
	}
	for (const auto value : cfg.dist_coeffs) {
		if (!std::isfinite(value)) {
			throw std::invalid_argument("preprocess.undistort.dist_coeffs contains non-finite value");
		}
	}
}

std::string validate_ipc_prefix(std::string prefix) {
	if (prefix.empty() || prefix.front() != '/') {
		throw std::invalid_argument("ipc.prefix must be an absolute path");
	}
	if (prefix.find('\\') != std::string::npos) {
		throw std::invalid_argument("ipc.prefix must not contain backslashes");
	}
	if (prefix.find("//") != std::string::npos) {
		throw std::invalid_argument("ipc.prefix must not contain empty path segments");
	}
	if (prefix.size() > 1 && prefix.back() == '/') {
		prefix.pop_back();
	}

	size_t start = 1;
	while (start <= prefix.size()) {
		auto end = prefix.find('/', start);
		if (end == std::string::npos) {
			end = prefix.size();
		}
		auto segment = prefix.substr(start, end - start);
		if (segment == "." || segment == "..") {
			throw std::invalid_argument("ipc.prefix must not contain traversal segments");
		}
		if (end == prefix.size()) {
			break;
		}
		start = end + 1;
	}

	return prefix;
}

namespace {

std::string make_config_target_uri(const app::Config &config) {
	return cvmmap::format(
		"cvmmap://{}@{}?namespace={}",
		config.name,
		config.ipc.prefix,
		config.ipc.name_space);
}

} // namespace

void validate_ipc_config(const app::Config &config) {
	try {
		(void)cvmmap::resolve_cvmmap_target_or_throw(make_config_target_uri(config));
	} catch (const std::exception &e) {
		throw std::invalid_argument(std::string("invalid cvmmap target configuration: ") + e.what());
	}
}

void validate_zed_body_tracking_config(
	const app::ZedConfig::BodyTrackingConfig &cfg) {
	if (!std::isfinite(cfg.max_range)) {
		throw std::invalid_argument("zed.body_tracking.max_range must be finite");
	}
	if (cfg.max_range != -1.0f && cfg.max_range <= 0.0f) {
		throw std::invalid_argument("zed.body_tracking.max_range must be -1 or positive");
	}
	if (!std::isfinite(cfg.prediction_timeout_s) || cfg.prediction_timeout_s < 0.0f) {
		throw std::invalid_argument("zed.body_tracking.prediction_timeout_s must be finite and non-negative");
	}
	if (!std::isfinite(cfg.detection_confidence_threshold) ||
		cfg.detection_confidence_threshold < 1.0f ||
		cfg.detection_confidence_threshold > 100.0f) {
		throw std::invalid_argument("zed.body_tracking.detection_confidence_threshold must be in [1, 100]");
	}
	if (cfg.minimum_keypoints_threshold < 0) {
		throw std::invalid_argument("zed.body_tracking.minimum_keypoints_threshold must be non-negative");
	}
	if (!std::isfinite(cfg.skeleton_smoothing) ||
		cfg.skeleton_smoothing < 0.0f ||
		cfg.skeleton_smoothing > 1.0f) {
		throw std::invalid_argument("zed.body_tracking.skeleton_smoothing must be in [0, 1]");
	}
	if (cfg.body_format == "BODY_34" && !cfg.enable_body_fitting) {
		throw std::invalid_argument("zed.body_tracking.enable_body_fitting must be true when body_format=BODY_34");
	}
	if (cfg.set_floor_as_origin && cfg.reference_frame != "WORLD") {
		spdlog::warn(
			"zed.body_tracking.set_floor_as_origin=true is most useful with "
			"zed.body_tracking.reference_frame=\"WORLD\"; current value is \"{}\"",
			cfg.reference_frame);
	}
}

void validate_zed_recording_config(const app::ZedConfig::RecordingConfig &cfg) {
	(void)canonicalize_zed_recording_compression_mode(cfg.compression_mode);
}

void validate_zed_runtime_config(const app::ZedConfig &cfg) {
	if (cfg.depth_max_fps < 0) {
		throw std::invalid_argument("zed.depth_max_fps must be non-negative");
	}
	if (cfg.depth_stabilization < 0 || cfg.depth_stabilization > 100) {
		throw std::invalid_argument("zed.depth_stabilization must be in [0, 100]");
	}
	if (cfg.depth_max_fps > 0 && cfg.body_tracking && cfg.body_tracking->enabled) {
		throw std::invalid_argument(
			"zed.depth_max_fps requires zed.body_tracking.enabled = false");
	}
}

} // namespace app::config_detail
