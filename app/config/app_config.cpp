#include "app_config.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <cmath>
#include <unordered_set>
#include <ranges>
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

std::string normalize_ascii_lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

std::string trim_ascii_spaces(std::string value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return "";
	}
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool is_valid_ipc_token_char(const char ch) {
	return std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-';
}

bool is_valid_ipc_token(const std::string &value, const size_t max_len) {
	if (value.empty() || value.size() > max_len) {
		return false;
	}
	if (!std::isalnum(static_cast<unsigned char>(value.front()))) {
		return false;
	}
	return std::ranges::all_of(value, is_valid_ipc_token_char);
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

void validate_ipc_config(const app::Config &config) {
	if (!is_valid_ipc_token(config.ipc.name_space, 32)) {
		throw std::invalid_argument("ipc.namespace must match [A-Za-z0-9][A-Za-z0-9._-]{0,31}");
	}
	if (!is_valid_ipc_token(config.name, 23)) {
		throw std::invalid_argument("name must match [A-Za-z0-9][A-Za-z0-9._-]{0,22}");
	}

	auto control_path = config.ipc.prefix + "/" + config.shm_name() + "_control";
	if (control_path.size() > 107) {
		throw std::invalid_argument(
			"ipc derived control path too long (>107 chars): " + control_path);
	}
}

bool is_zed_network_stream_mode(const std::string_view mode) {
	const auto normalized = normalize_ascii_lower(std::string(mode));
	return normalized == "network" || normalized == "ethernet" || normalized == "stream";
}

bool is_zed_local_stream_mode(const std::string_view mode) {
	const auto normalized = normalize_ascii_lower(std::string(mode));
	return normalized == "local" || normalized == "usb" || normalized == "device" || normalized == "auto";
}

bool is_valid_zed_stream_mode(const std::string_view mode) {
	return is_zed_network_stream_mode(mode) || is_zed_local_stream_mode(mode);
}

std::string canonical_zed_stream_mode(const std::string_view mode) {
	return is_zed_network_stream_mode(mode) ? "network" : "local";
}

std::string validate_and_canonicalize_zed_resolution(const std::string_view resolution) {
	const auto normalized = normalize_ascii_lower(std::string(resolution));

	if (normalized == "2k") {
		return "HD2K";
	}
	if (normalized == "1080p" || normalized == "fhd") {
		return "HD1080";
	}
	if (normalized == "720p" || normalized == "hd") {
		return "HD720";
	}

	static const std::unordered_set<std::string> valid_resolutions = {
		"hd2k", "hd1200", "hd1080", "hd720", "svga", "vga", "auto"};

	if (valid_resolutions.find(normalized) != valid_resolutions.end()) {
		std::string result = normalized;
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return result;
	}

	throw std::invalid_argument(
		"invalid zed.resolution: '" + std::string(resolution) +
		"'. Allowed values: HD2K, HD1200, HD1080, HD720, SVGA, VGA, AUTO "
		"(aliases: 2k, 1080p, fhd, 720p, hd)");
}

std::string validate_and_canonicalize_zed_depth_mode(const std::string_view depth_mode) {
	auto normalized = normalize_ascii_lower(std::string(depth_mode));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	static const std::unordered_set<std::string> valid_depth_modes = {
		"none", "neural", "neural_light", "neural_plus"};

	if (valid_depth_modes.find(normalized) != valid_depth_modes.end()) {
		std::string result = normalized;
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return result;
	}

	throw std::invalid_argument(
		"invalid zed.depth_mode: '" + std::string(depth_mode) +
		"'. Allowed values: NONE, NEURAL, NEURAL_LIGHT, NEURAL_PLUS "
		"(aliases: neural light, neural-light, neural plus, neural-plus)");
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
	case BackendType::ZED:
		return "zed";
	default:
		return "unknown";
	}
}

BackendType backend_from_string(std::string_view s) {
	if (s == "opencv" || s == "OpenCV") {
		return BackendType::OpenCV;
	} else if (s == "gstreamer" || s == "GStreamer" || s == "gst") {
		return BackendType::GStreamer;
	} else if (s == "zed" || s == "ZED") {
		return BackendType::ZED;
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

	if (auto ipc = tbl["ipc"].as_table(); ipc) {
		if (auto val = (*ipc)["namespace"].value<std::string>(); val) {
			config.ipc.name_space = trim_ascii_spaces(*val);
		} else {
			config.ipc.name_space = "cvmmap";
		}

		if (auto val = (*ipc)["prefix"].value<std::string>(); val) {
			config.ipc.prefix = validate_ipc_prefix(trim_ascii_spaces(*val));
		} else {
			config.ipc.prefix = "/tmp";
		}
	} else {
		config.ipc.name_space = "cvmmap";
		config.ipc.prefix = "/tmp";
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

	if (auto zed = tbl["zed"].as_table(); zed) {
		ZedConfig zed_cfg{};

		if (auto val = (*zed)["stream_mode"].value<std::string>(); val) {
			zed_cfg.stream_mode = normalize_ascii_lower(trim_ascii_spaces(*val));
		} else {
			zed_cfg.stream_mode = "local";
		}

		if (!is_valid_zed_stream_mode(zed_cfg.stream_mode)) {
			throw invalid_argument("zed.stream_mode must be one of: local, usb, device, auto, network, ethernet, stream");
		}

		if (auto val = (*zed)["serial"]; val) {
			if (auto serial = val.value<int>(); serial) {
				if (*serial < 0) {
					throw invalid_argument("zed.serial must be non-negative");
				}
				zed_cfg.serial = *serial;
			} else {
				throw invalid_argument("zed.serial must be integer");
			}
		}

		if (auto val = (*zed)["index"]; val) {
			if (auto index = val.value<int>(); index) {
				if (*index < 0) {
					throw invalid_argument("zed.index must be non-negative");
				}
				zed_cfg.index = *index;
			} else {
				throw invalid_argument("zed.index must be integer");
			}
		}

		if (auto val = (*zed)["ip_address"]; val) {
			if (auto ip = val.value<std::string>(); ip) {
				auto trimmed_ip = trim_ascii_spaces(*ip);
				if (trimmed_ip.empty()) {
					throw invalid_argument("zed.ip_address must not be empty when provided");
				}
				zed_cfg.ip_address = std::move(trimmed_ip);
			} else {
				throw invalid_argument("zed.ip_address must be string");
			}
		}

		if (auto val = (*zed)["port"]; val) {
			if (auto port = val.value<int>(); port) {
				if (*port <= 0 || *port > 65535) {
					throw invalid_argument("zed.port must be in range 1..65535");
				}
				zed_cfg.port = *port;
			} else {
				throw invalid_argument("zed.port must be integer");
			}
		}

		if (auto val = (*zed)["resolution"].value<std::string>(); val) {
			zed_cfg.resolution = validate_and_canonicalize_zed_resolution(*val);
		} else {
			throw invalid_argument("zed.resolution is required when [zed] section exists");
		}

		if (auto val = (*zed)["fps"].value<int>(); val) {
			zed_cfg.fps = *val;
		} else {
			throw invalid_argument("zed.fps is required when [zed] section exists and must be integer");
		}

		if (auto val = (*zed)["depth_mode"].value<std::string>(); val) {
			zed_cfg.depth_mode = validate_and_canonicalize_zed_depth_mode(*val);
		} else {
			throw invalid_argument("zed.depth_mode is required when [zed] section exists");
		}

		if (auto val = (*zed)["open_timeout_ms"]; val) {
			if (auto v = val.value<int>(); v) {
				zed_cfg.open_timeout_ms = *v;
			} else {
				throw invalid_argument("zed.open_timeout_ms must be integer");
			}
		}

		if (auto val = (*zed)["warmup_frames"]; val) {
			if (auto v = val.value<int>(); v) {
				zed_cfg.warmup_frames = *v;
			} else {
				throw invalid_argument("zed.warmup_frames must be integer");
			}
		}

		if (auto val = (*zed)["max_consecutive_failures"]; val) {
			if (auto v = val.value<int>(); v) {
				zed_cfg.max_consecutive_failures = *v;
			} else {
				throw invalid_argument("zed.max_consecutive_failures must be integer");
			}
		}

		if (auto val = (*zed)["reconnect_interval_ms"]; val) {
			if (auto v = val.value<int>(); v) {
				zed_cfg.reconnect_interval_ms = *v;
			} else {
				throw invalid_argument("zed.reconnect_interval_ms must be integer");
			}
		}

		if (auto val = (*zed)["reconnect"]; val) {
			if (auto v = val.value<bool>(); v) {
				zed_cfg.reconnect = *v;
			} else {
				throw invalid_argument("zed.reconnect must be boolean");
			}
		}

		if (auto val = (*zed)["left_pixel_format"].value<std::string>(); val) {
			zed_cfg.left_pixel_format = normalize_ascii_lower(*val);
		} else {
			zed_cfg.left_pixel_format = "bgr8";
		}

		if (zed_cfg.serial && zed_cfg.index) {
			throw invalid_argument("zed.serial and zed.index are mutually exclusive");
		}

		if (is_zed_network_stream_mode(zed_cfg.stream_mode)) {
			if (!zed_cfg.ip_address || zed_cfg.ip_address->empty()) {
				throw invalid_argument("zed.ip_address is required when zed.stream_mode is network/ethernet/stream");
			}
			if (zed_cfg.serial || zed_cfg.index) {
				throw invalid_argument("zed.serial and zed.index must not be set when zed.stream_mode is network/ethernet/stream");
			}
		} else {
			if (zed_cfg.ip_address) {
				throw invalid_argument("zed.ip_address is only valid when zed.stream_mode is network/ethernet/stream");
			}
			if (zed_cfg.port) {
				throw invalid_argument("zed.port is only valid when zed.stream_mode is network/ethernet/stream");
			}
		}

		zed_cfg.stream_mode = canonical_zed_stream_mode(zed_cfg.stream_mode);

		config.zed = zed_cfg;
	}

	// Validate: ensure the selected backend has its config
	if (config.video.backend == BackendType::OpenCV && !config.opencv) {
		throw invalid_argument("[opencv] section is required when backend is 'opencv'");
	}
	if (config.video.backend == BackendType::GStreamer && !config.gstreamer) {
		throw invalid_argument("[gstreamer] section is required when backend is 'gstreamer'");
	}
	if (config.video.backend == BackendType::ZED && !config.zed) {
		throw invalid_argument("[zed] section is required when backend is 'zed'");
	}

	validate_ipc_config(config);

	return config;
}

std::string Config::to_toml() const {
	std::ostringstream ss;
	ss << "name = \"" << name << "\"\n\n";
	ss << "[ipc]\n";
	ss << "namespace = \"" << ipc.name_space << "\"\n";
	ss << "prefix = \"" << ipc.prefix << "\"\n\n";

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

	if (zed) {
		ss << "\n[zed]\n";
		ss << "stream_mode = \"" << canonical_zed_stream_mode(zed->stream_mode) << "\"\n";
		if (zed->ip_address) {
			ss << "ip_address = \"" << *zed->ip_address << "\"\n";
		}
		if (zed->port) {
			ss << "port = " << *zed->port << "\n";
		}
		if (zed->serial) {
			ss << "serial = " << *zed->serial << "\n";
		}
		if (zed->index) {
			ss << "index = " << *zed->index << "\n";
		}
		ss << "resolution = \"" << zed->resolution << "\"\n";
		ss << "fps = " << zed->fps << "\n";
		ss << "depth_mode = \"" << zed->depth_mode << "\"\n";
		ss << "open_timeout_ms = " << zed->open_timeout_ms << "\n";
		ss << "warmup_frames = " << zed->warmup_frames << "\n";
		ss << "max_consecutive_failures = " << zed->max_consecutive_failures << "\n";
		ss << "reconnect_interval_ms = " << zed->reconnect_interval_ms << "\n";
		ss << "reconnect = " << (zed->reconnect ? "true" : "false") << "\n";
		ss << "left_pixel_format = \"" << zed->left_pixel_format << "\"\n";
	}

	return ss.str();
}
} // namespace app
