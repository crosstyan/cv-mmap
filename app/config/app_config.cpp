#include "app_config.hpp"
#include <algorithm>
#include <sstream>
#include <cmath>
#include <toml++/toml.hpp>
#include <spdlog/spdlog.h>

namespace {
/**
 * @brief Normalize a pipeline string by replacing newlines with spaces.
 *
 * GStreamer's gst_parse_launch doesn't handle multiline strings properly.
 * This function converts newlines to spaces to make the pipeline valid.
 *
 * @param pipeline The pipeline string potentially containing newlines
 * @return std::string The normalized single-line pipeline string
 */
std::string normalize_pipeline_string(std::string pipeline) {
	pipeline.erase(std::remove(pipeline.begin(), pipeline.end(), '\r'), pipeline.end());
	std::replace(pipeline.begin(), pipeline.end(), '\n', ' ');
	return pipeline;
}

std::array<double, 9> parse_camera_matrix(const toml::node_view<toml::node> &node) {
	auto arr = node.as_array();
	if (!arr) {
		throw std::invalid_argument("preprocess.undistort.camera_matrix must be an array");
	}
	if (arr->size() != 9) {
		throw std::invalid_argument("preprocess.undistort.camera_matrix must have exactly 9 values");
	}

	std::array<double, 9> values{};
	for (size_t i = 0; i < 9; ++i) {
		auto value = (*arr)[i].value<double>();
		if (!value) {
			throw std::invalid_argument("preprocess.undistort.camera_matrix must contain only numeric values");
		}
		values[i] = *value;
	}
	return values;
}

std::vector<double> parse_dist_coeffs(const toml::node_view<toml::node> &node) {
	auto arr = node.as_array();
	if (!arr) {
		throw std::invalid_argument("preprocess.undistort.dist_coeffs must be an array");
	}
	std::vector<double> values;
	values.reserve(arr->size());
	for (const auto &entry : *arr) {
		auto value = entry.value<double>();
		if (!value) {
			throw std::invalid_argument("preprocess.undistort.dist_coeffs must contain only numeric values");
		}
		values.push_back(*value);
	}
	return values;
}

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
} // namespace

namespace app {
using invalid_argument = std::invalid_argument;

std::string_view to_string(BackendType backend) {
	switch (backend) {
	case BackendType::OpenCV:
		return "opencv";
	case BackendType::GStreamer:
		return "gstreamer";
	default:
		return "unknown";
	}
}

BackendType backend_from_string(std::string_view s) {
	if (s == "opencv" || s == "OpenCV") {
		return BackendType::OpenCV;
	} else if (s == "gstreamer" || s == "GStreamer" || s == "gst") {
		return BackendType::GStreamer;
	}
	throw invalid_argument("unknown backend type: " + std::string(s));
}

std::string_view to_string(FiniteStreamEndingBehavior behavior) {
	switch (behavior) {
	case FiniteStreamEndingBehavior::Stop:
		return "stop";
	case FiniteStreamEndingBehavior::Loop:
		return "loop";
	default:
		return "unknown";
	}
}

FiniteStreamEndingBehavior finite_stream_ending_behavior_from_string(std::string_view s) {
	if (s == "stop" || s == "Stop") {
		return FiniteStreamEndingBehavior::Stop;
	} else if (s == "loop" || s == "Loop") {
		return FiniteStreamEndingBehavior::Loop;
	}
	throw invalid_argument("unknown finite_stream_ending_behavior: " + std::string(s));
}

std::string_view to_string(UndistortModel model) {
	switch (model) {
	case UndistortModel::Pinhole:
		return "pinhole";
	default:
		return "unknown";
	}
}

UndistortModel undistort_model_from_string(std::string_view s) {
	if (s == "pinhole" || s == "Pinhole") {
		return UndistortModel::Pinhole;
	}
	throw invalid_argument("unknown undistort model: " + std::string(s));
}

Config Config::Default() {
	return {
		.name  = "default",
		.video = VideoConfig{
			.backend                       = BackendType::GStreamer,
			.use_finite_as_infinite_stream = false,
			.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Stop,
		},
		.opencv    = std::nullopt,
		.gstreamer = GStreamerConfig{
			.pipeline = "videotestsrc ! timeoverlay ! videoconvert ! video/x-raw,format=BGR ! appsink name=opencvsink",
		},
		.preprocess = std::nullopt,
	};
}

Config Config::from_toml(const std::filesystem::path &path) {
	toml::table tbl;
	try {
		tbl = toml::parse_file(path.string());
	} catch (const toml::parse_error &e) {
		spdlog::error("failed to parse config file: {}", e.what());
		throw;
	}

	Config config{};

	// name (required)
	if (auto val = tbl["name"].value<std::string>(); val) {
		config.name = *val;
	} else {
		throw invalid_argument("name is required");
	}

	// [video] section
	if (auto video = tbl["video"].as_table(); video) {
		// backend
		if (auto val = (*video)["backend"].value<std::string>(); val) {
			config.video.backend = backend_from_string(*val);
		} else {
			config.video.backend = BackendType::OpenCV; // default
		}

		// use_finite_as_infinite_stream
		config.video.use_finite_as_infinite_stream = (*video)["use_finite_as_infinite_stream"].value_or(false);

		// finite_stream_ending_behavior
		if (auto val = (*video)["finite_stream_ending_behavior"].value<std::string>(); val) {
			config.video.finite_stream_ending_behavior = finite_stream_ending_behavior_from_string(*val);
		} else {
			config.video.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Stop;
		}
	}

	// [opencv] section
	if (auto opencv = tbl["opencv"].as_table(); opencv) {
		OpenCVConfig opencv_cfg{};

		// parameter: string or int
		if (auto node = (*opencv)["parameter"]; node) {
			if (auto s = node.value<std::string>(); s) {
				opencv_cfg.parameter = *s;
			} else if (auto i = node.value<int>(); i) {
				opencv_cfg.parameter = *i;
			} else {
				throw invalid_argument("opencv.parameter must be string or integer");
			}
		} else {
			throw invalid_argument("opencv.parameter is required when [opencv] section exists");
		}

		// api_preference (optional)
		if (auto val = (*opencv)["api"].value<std::string>(); val) {
			opencv_cfg.api_preference = from_string(*val);
		} else {
			opencv_cfg.api_preference = CAP_ANY;
		}

		config.opencv = opencv_cfg;
	}

	// [gstreamer] section
	if (auto gst = tbl["gstreamer"].as_table(); gst) {
		GStreamerConfig gst_cfg{};

		// pipeline (required)
		if (auto val = (*gst)["pipeline"].value<std::string>(); val) {
			gst_cfg.pipeline = normalize_pipeline_string(*val);
		} else {
			throw invalid_argument("gstreamer.pipeline is required when [gstreamer] section exists");
		}

		config.gstreamer = gst_cfg;
	}

	if (auto preprocess = tbl["preprocess"].as_table(); preprocess) {
		PreprocessConfig preprocess_cfg{};
		if (auto undistort = (*preprocess)["undistort"].as_table(); undistort) {
			UndistortConfig undistort_cfg{};
			undistort_cfg.enabled = (*undistort)["enabled"].value_or(false);
			if (auto model = (*undistort)["model"].value<std::string>(); model) {
				undistort_cfg.model = undistort_model_from_string(*model);
			}
			if (auto camera_matrix = (*undistort)["camera_matrix"]; camera_matrix) {
				undistort_cfg.camera_matrix = parse_camera_matrix(camera_matrix);
			}
			if (auto dist_coeffs = (*undistort)["dist_coeffs"]; dist_coeffs) {
				undistort_cfg.dist_coeffs = parse_dist_coeffs(dist_coeffs);
			}
			undistort_cfg.use_optimal_new_camera_matrix = (*undistort)["use_optimal_new_camera_matrix"].value_or(true);
			undistort_cfg.alpha                         = (*undistort)["alpha"].value_or(0.0);
			undistort_cfg.crop_to_valid_roi             = (*undistort)["crop_to_valid_roi"].value_or(false);
			undistort_cfg.strict_startup                = (*undistort)["strict_startup"].value_or(false);

			if (undistort_cfg.enabled) {
				try {
					validate_undistort_config(undistort_cfg);
				} catch (const std::exception &e) {
					if (undistort_cfg.strict_startup) {
						throw;
					}
					spdlog::warn("invalid undistort config; disabling pass: {}", e.what());
					undistort_cfg.enabled = false;
				}
			}
			preprocess_cfg.undistort = std::move(undistort_cfg);
		}
		config.preprocess = std::move(preprocess_cfg);
	}

	// Validate: ensure the selected backend has its config
	if (config.video.backend == BackendType::OpenCV && !config.opencv) {
		throw invalid_argument("[opencv] section is required when backend is 'opencv'");
	}
	if (config.video.backend == BackendType::GStreamer && !config.gstreamer) {
		throw invalid_argument("[gstreamer] section is required when backend is 'gstreamer'");
	}

	return config;
}

std::string Config::to_toml() const {
	std::ostringstream ss;
	ss << "name = \"" << name << "\"\n\n";

	ss << "[video]\n";
	ss << "backend = \"" << to_string(video.backend) << "\"\n";
	ss << "use_finite_as_infinite_stream = " << (video.use_finite_as_infinite_stream ? "true" : "false") << "\n";
	ss << "finite_stream_ending_behavior = \"" << to_string(video.finite_stream_ending_behavior) << "\"\n\n";

	if (opencv) {
		ss << "[opencv]\n";
		ss << "parameter = ";
		if (std::holds_alternative<std::string>(opencv->parameter)) {
			ss << "\"" << std::get<std::string>(opencv->parameter) << "\"";
		} else {
			ss << std::get<int>(opencv->parameter);
		}
		ss << "\n";
		ss << "api = \"" << to_string(opencv->api_preference) << "\"\n\n";
	}

	if (gstreamer) {
		ss << "[gstreamer]\n";
		ss << "pipeline = \"" << gstreamer->pipeline << "\"\n";
	}

	if (preprocess && preprocess->undistort) {
		const auto &undistort = *preprocess->undistort;
		ss << "\n[preprocess.undistort]\n";
		ss << "enabled = " << (undistort.enabled ? "true" : "false") << "\n";
		ss << "model = \"" << to_string(undistort.model) << "\"\n";
		ss << "camera_matrix = [";
		for (size_t i = 0; i < undistort.camera_matrix.size(); ++i) {
			if (i != 0) {
				ss << ", ";
			}
			ss << undistort.camera_matrix[i];
		}
		ss << "]\n";
		ss << "dist_coeffs = [";
		for (size_t i = 0; i < undistort.dist_coeffs.size(); ++i) {
			if (i != 0) {
				ss << ", ";
			}
			ss << undistort.dist_coeffs[i];
		}
		ss << "]\n";
		ss << "use_optimal_new_camera_matrix = " << (undistort.use_optimal_new_camera_matrix ? "true" : "false") << "\n";
		ss << "alpha = " << undistort.alpha << "\n";
		ss << "crop_to_valid_roi = " << (undistort.crop_to_valid_roi ? "true" : "false") << "\n";
		ss << "strict_startup = " << (undistort.strict_startup ? "true" : "false") << "\n";
	}

	return ss.str();
}
} // namespace app
