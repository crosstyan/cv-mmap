#include "app_config.hpp"
#include <sstream>
#include <toml++/toml.hpp>
#include <spdlog/spdlog.h>

namespace app {
using invalid_argument = std::invalid_argument;
Config Config::Default() {
	return {
		.name           = "default",
		.pipeline       = "videotestsrc ! timeoverlay ! videoconvert ! video/x-raw,format=BGR ! appsink name=opencvsink",
		.api_preference = CAP_GSTREAMER,
		.zmq_address    = "ipc:///tmp/0",
		.is_loop        = false,
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
	// name
	if (auto val = tbl["name"].value<std::string>(); val) {
		config.name = *val;
	} else {
		throw invalid_argument("name is required");
	}

	// pipeline: string or int
	if (auto node = tbl["pipeline"]; node) {
		if (auto s = node.value<std::string>(); s) {
			config.pipeline = *s;
		} else if (auto i = node.value<int>(); i) {
			config.pipeline = *i;
		} else {
			throw invalid_argument("pipeline must be string or integer");
		}
	} else {
		throw invalid_argument("pipeline is required");
	}

	// api preference
	if (auto val = tbl["api"].value<std::string>(); val) {
		config.api_preference = from_string(*val);
	} else {
		config.api_preference = CAP_ANY;
	}

	// zmq address
	if (auto val = tbl["zmq_address"].value<std::string>(); val) {
		config.zmq_address = *val;
	} else {
		throw invalid_argument("zmq_address is required");
	}

	// is_loop
	config.is_loop = tbl["is_loop"].value_or(false);

	return config;
}

std::string Config::to_toml() {
	std::ostringstream ss;
	ss << "name = \"" << name << "\"\n";
	ss << "pipeline = ";
	if (std::holds_alternative<std::string>(pipeline)) {
		ss << "\"" << std::get<std::string>(pipeline) << "\"";
	} else {
		ss << std::get<int>(pipeline);
	}
	ss << "\n";
	ss << "api = \"" << to_string(api_preference) << "\"\n";
	ss << "zmq_address = \"" << zmq_address << "\"\n";
	ss << "is_loop = " << (is_loop ? "true" : "false") << "\n";
	return ss.str();
}
}