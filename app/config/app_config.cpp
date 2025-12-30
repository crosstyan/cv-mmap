#include "app_config.hpp"
#include <sstream>
#include <toml++/toml.hpp>
#include <spdlog/spdlog.h>

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

Config Config::Default() {
	return {
		.name                          = "default",
		.backend                       = BackendType::GStreamer,
		.use_finite_as_infinite_stream = false,
		.opencv                        = std::nullopt,
		.gstreamer                     = GStreamerConfig{
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
			config.backend = backend_from_string(*val);
		} else {
			config.backend = BackendType::OpenCV; // default
		}

		// use_finite_as_infinite_stream
		config.use_finite_as_infinite_stream = (*video)["use_finite_as_infinite_stream"].value_or(false);
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
			gst_cfg.pipeline = *val;
		} else {
			throw invalid_argument("gstreamer.pipeline is required when [gstreamer] section exists");
		}

		config.gstreamer = gst_cfg;
	}

	// Validate: ensure the selected backend has its config
	if (config.backend == BackendType::OpenCV && !config.opencv) {
		throw invalid_argument("[opencv] section is required when backend is 'opencv'");
	}
	if (config.backend == BackendType::GStreamer && !config.gstreamer) {
		throw invalid_argument("[gstreamer] section is required when backend is 'gstreamer'");
	}

	return config;
}

std::string Config::to_toml() const {
	std::ostringstream ss;
	ss << "name = \"" << name << "\"\n\n";

	ss << "[video]\n";
	ss << "backend = \"" << to_string(backend) << "\"\n";
	ss << "use_finite_as_infinite_stream = " << (use_finite_as_infinite_stream ? "true" : "false") << "\n\n";

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

	return ss.str();
}
}