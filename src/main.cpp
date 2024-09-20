#include <iostream>
#include <filesystem>
#include <format>
#include <map>
#include <unordered_map>
#include <string_view>
#include <CLI/CLI.hpp>
#include <charconv>
#include <toml++/toml.hpp>
#include <spdlog/spdlog.h>
#include <opencv2/opencv.hpp>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#ifndef GIT_REV
#define GIT_REV "N/A"
#endif
#ifndef GIT_TAG
#define GIT_TAG ""
#endif
#ifndef GIT_BRANCH
#define GIT_BRANCH "N/A"
#endif
#ifndef COMPILE_TIMESTAMP
#define COMPILE_TIMESTAMP "1970-01-01T00:00:00"
#endif

#define STRR(X) #X
#define STR(X)  STRR(X)

namespace app {
using invalid_argument = std::invalid_argument;

constexpr std::string_view trim(std::string_view s) {
	s.remove_prefix(std::min(s.find_first_not_of(" \t\r\v\n"), s.size()));
	s.remove_suffix(std::min(s.size() - s.find_last_not_of(" \t\r\v\n") - 1, s.size()));
	return s;
}

using cap_api_t = decltype(cv::CAP_ANY);

static const std::map<std::string, cap_api_t> api_map = {
	{"any", cv::CAP_ANY},
	{"v4l", cv::CAP_V4L},
	{"v4l2", cv::CAP_V4L2},
	{"gstreamer", cv::CAP_GSTREAMER},
	{"dshow", cv::CAP_DSHOW},
	{"avfoundation", cv::CAP_AVFOUNDATION},
	{"ffmpeg", cv::CAP_FFMPEG},
};

std::string_view cap_api_to_string(const cap_api_t api) {
	for (const auto &[key, value] : api_map) {
		if (value == api) {
			return key;
		}
	}
	throw invalid_argument(std::format("Invalid API value: `{}`", static_cast<int>(api)));
}

cap_api_t cap_api_from_string(const std::string_view s) {
	for (const auto &[key, value] : api_map) {
		if (key == s) {
			return value;
		}
	}
	throw invalid_argument(std::format("Invalid API key: `{}`", s));
}

std::variant<int, std::string> pipeline_or_index(const std::string &s) {
	int val;
	if (const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val, 10); ec == std::errc{}) {
		return val;
	} else {
		return s;
	}
}

struct Config {
	std::string name;
	// pipeline or index, depends on API
	std::variant<std::string, int> pipeline;
	cap_api_t api_preference;
	std::string zmq_address;

	static Config Default() {
		// https://github.com/opencv/opencv/blob/f503890c2b2ba73f4f94971c1845ead941143262/modules/videoio/src/cap_gstreamer.cpp#L1318-L1322
		return {
			.name           = "default",
			.pipeline       = "videotestsrc ! videoconvert ! video/x-raw,format=BGR ! appsink name=sink",
			.api_preference = cv::CAP_GSTREAMER,
			.zmq_address    = "ipc:///tmp/0",
		};
	}

	static Config from_toml(const toml::table &table) {
		Config config;
		if (const auto name = table["name"]; name) {
			config.name = *name.value<std::string>();
		} else {
			throw invalid_argument("name is required");
		}
		if (const auto pipeline = table["pipeline"]; pipeline) {
			if (const auto s = pipeline.value<std::string>(); s) {
				config.pipeline = *s;
			} else if (const auto i = pipeline.value<int>(); i) {
				config.pipeline = *i;
			} else {
				throw invalid_argument("pipeline must be string or integer");
			}
		} else {
			throw invalid_argument("pipeline is required");
		}
		if (const auto api = table["api"]; api) {
			config.api_preference = cap_api_from_string(*api.value<std::string>());
		} else {
			throw invalid_argument("api is required");
		}
		if (const auto zmq_address = table["zmq_address"]; zmq_address) {
			config.zmq_address = *zmq_address.value<std::string>();
		} else {
			throw invalid_argument("zmq_address is required");
		}
		return config;
	}

	[[nodiscard]]
	std::string to_toml() const {
		auto ss  = std::stringstream{};
		auto tbl = toml::table{
			{"name", name},
			{"api", cap_api_to_string(api_preference)},
			{"zmq_address", zmq_address},
		};
		if (std::holds_alternative<int>(pipeline)) {
			tbl.insert_or_assign("pipeline", std::get<int>(pipeline));
		} else {
			tbl.insert_or_assign("pipeline", std::get<std::string>(pipeline));
		}
		ss << tbl << "\n\n";
		return ss.str();
	}
};
}


namespace app::version {
constexpr auto revision          = STR(GIT_REV);
constexpr auto tag               = STR(GIT_TAG);
constexpr auto trim_tag          = trim(tag);
constexpr auto branch            = STR(GIT_BRANCH);
constexpr auto compile_timestamp = STR(COMPILE_TIMESTAMP);

void print_version() {
	if (constexpr auto t = std::string_view{tag}; t.empty()) {
		std::cout << "Version: " << revision << " (" << branch << ")\n";
	} else {
		std::cout << "Version: " << tag << " (" << revision << ")\n";
	}
}
}

int main(int argc, char **argv) {
	app::version::print_version();

	CLI::App app{"Video Stream mmap adapter"};
	argv = app.ensure_utf8(argv);
	// default config file name is config.toml in cwd
	std::string config_file = "config.toml";
	app.add_option("-c,--config", config_file, "Config file path");
	bool use_default = false;
	app.add_flag("--default", use_default, "Use default config");
	CLI11_PARSE(app, argc, argv);

	const std::filesystem::path config_path = config_file;
	if (not std::filesystem::exists(config_path)) {
		if (use_default) {
			std::ofstream ofs(config_file);
			ofs << app::Config::Default().to_toml();
			ofs.close();
			spdlog ::info("Create default config file in `{}`; Please restart the program.", config_file);
			return 0;
		} else {
			spdlog::error("Config file not found in `{}`. Use --default to create a default config", config_file);
			return 1;
		}
	}
	toml::table config_tbl;
	try {
		config_tbl = toml::parse_file(config_file);
	} catch (const toml::parse_error &e) {
		spdlog::error("Failed to parse config file: {}", e.what());
		return 1;
	}
	app::Config config;
	try {
		config = app::Config::from_toml(config_tbl);
	} catch (const app::invalid_argument &e) {
		spdlog::error("Invalid config: {}", e.what());
		return 1;
	}

	std::cout << "Config: " << config.to_toml() << std::endl;
	cv::VideoCapture cap;
	// https://gstreamer.freedesktop.org/documentation/shm/shmsink.html?gi-language=c
	if (std::holds_alternative<int>(config.pipeline)) {
		const auto index = std::get<int>(config.pipeline);
		spdlog::info("Open video source index (int): {}", index);
		cap.open(index, config.api_preference);
	} else {
		const auto pipeline = std::get<std::string>(config.pipeline);
		spdlog::info("Open video source pipeline (string): {}", pipeline);
		cap.open(pipeline, config.api_preference);
	}
	if (not cap.isOpened()) {
		spdlog::error("Failed to open video source. Check OpenCV VideoCapture API support if you're sure the source is correct.");
		std::cout << cv::getBuildInformation() << std::endl;
		return 1;
	}
	for (;;) {
		cv::Mat frame;
		cap >> frame;
		if (frame.empty()) {
			spdlog::error("Failed to capture frame");
			break;
		} else {
			spdlog::info("Frame: {}x{}", frame.cols, frame.rows);
		}
	}
	return 0;
}
