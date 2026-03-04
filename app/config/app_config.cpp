#include "app_config.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
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
			zed_cfg.resolution = *val;
		} else {
			throw invalid_argument("zed.resolution is required when [zed] section exists");
		}

		if (auto val = (*zed)["fps"].value<int>(); val) {
			zed_cfg.fps = *val;
		} else {
			throw invalid_argument("zed.fps is required when [zed] section exists and must be integer");
		}

		if (auto val = (*zed)["depth_mode"].value<std::string>(); val) {
			zed_cfg.depth_mode = *val;
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
