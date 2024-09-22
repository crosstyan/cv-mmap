#include <atomic>
#include <iostream>
#include <filesystem>
#include <format>
#include <csignal>
#include <unordered_map>
#include <string_view>
#include <string>
#include <charconv>
#include <expected>
#include <span>
#include <CLI/CLI.hpp>
#include <toml++/toml.hpp>
#include <spdlog/spdlog.h>
#include <opencv2/videoio.hpp>
#include <zmq_addon.hpp>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

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

static const std::unordered_map<std::string, cap_api_t> api_map = {
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
	throw invalid_argument(std::format("invalid API value: `{}`", static_cast<int>(api)));
}

cap_api_t cap_api_from_string(const std::string_view s) {
	for (const auto &[key, value] : api_map) {
		if (key == s) {
			return value;
		}
	}
	throw invalid_argument(std::format("invalid API key: `{}`", s));
}

struct Config {
	/// name of shared memory (with `shm_open` and `shm_unlink`)
	std::string name;
	/// pipeline or index, depends on API
	std::variant<std::string, int> pipeline;
	/// API preference used by OpenCV
	cap_api_t api_preference = cv::CAP_ANY;
	/// ZMQ address for synchronization
	std::string zmq_address;

	static Config Default() {
		// https://github.com/opencv/opencv/blob/f503890c2b2ba73f4f94971c1845ead941143262/modules/videoio/src/cap_gstreamer.cpp#L1535
		// https://github.com/opencv/opencv/blob/f503890c2b2ba73f4f94971c1845ead941143262/modules/videoio/src/cap_gstreamer.cpp#L1503
		// an appsink called `opencvsink`
		return {
			.name           = "default",
			.pipeline       = "videotestsrc ! videoconvert ! video/x-raw,format=BGR ! appsink name=opencvsink",
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
		ss << tbl << "\r\n";
		return ss.str();
	}
};

std::string depth_to_string(const int depth) {
	switch (depth) {
	case CV_8U:
		return "CV_8U";
	case CV_8S:
		return "CV_8S";
	case CV_16U:
		return "CV_16U";
	case CV_16S:
		return "CV_16S";
	case CV_16F:
		return "CV_16F";
	case CV_32S:
		return "CV_32S";
	case CV_32F:
		return "CV_32F";
	case CV_64F:
		return "CV_64F";
	default:
		return "unknown";
	}
}

// https://gist.github.com/yangcha/38f2fa630e223a8546f9b48ebbb3e61a
inline int cv_depth_to_size(int depth) {
	switch (depth) {
	case CV_8U:
	case CV_8S:
		return 1;
	case CV_16U:
	case CV_16S:
	case CV_16F:
		return 2;
	case CV_32S:
	case CV_32F:
		return 4;
	case CV_64F:
		return 8;
	default:
		throw app::invalid_argument(std::format("invalid depth value `{}`", depth));
	}
}

// https://docs.opencv.org/4.x/d3/d63/classcv_1_1Mat.html
// See `Detailed Description`
// strides for each dimension
// stride[0]=channel
// stride[1]=channel*cols
// stride[2]=channel*cols*rows
struct __attribute__((packed)) frame_info_t {
	uint16_t width;
	uint16_t height;
	/// CV_8U, CV_8S, CV_16U, CV_16S, CV_16F, CV_32S, CV_32F, CV_64F
	uint8_t depth;
	uint32_t bufferSize;

	[[nodiscard]]
	int pixelWidth() const {
		return cv_depth_to_size(depth);
	}

	int marshal(std::span<uint8_t> buf) const {
		if (buf.size() < sizeof(frame_info_t)) {
			return -1;
		}
		memcpy(buf.data(), this, sizeof(frame_info_t));
		return sizeof(frame_info_t);
	}

	static std::optional<frame_info_t> unmarshal(const std::span<uint8_t> buf) {
		if (buf.size() < sizeof(frame_info_t)) {
			return std::nullopt;
		}
		frame_info_t info;
		memcpy(&info, buf.data(), sizeof(frame_info_t));
		return info;
	}
};

struct __attribute__((packed)) sync_message_t {
	uint32_t frame_count;
	frame_info_t info;
	// NOTE: I don't need the `name` field
	// as long as we don't share same IPC socket for different video sources.
	int marshal(std::span<uint8_t> buf) const {
		if (buf.size() < sizeof(sync_message_t)) {
			return -1;
		}
		memcpy(buf.data(), this, sizeof(sync_message_t));
		return sizeof(sync_message_t);
	}

	static std::optional<sync_message_t> unmarshal(const std::span<uint8_t> buf) {
		if (buf.size() < sizeof(sync_message_t)) {
			return std::nullopt;
		}
		sync_message_t msg;
		memcpy(&msg, buf.data(), sizeof(sync_message_t));
		return msg;
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
	if constexpr (constexpr auto t = std::string_view{tag}; t.empty()) {
		std::cout << "version: " << revision << " (" << branch << ")\n";
	} else {
		std::cout << "version: " << tag << " (" << revision << ")\n";
	}
}
}

int main(int argc, char **argv) {
	using namespace app;
	app::version::print_version();

	CLI::App app{"Video Stream mmap adapter"};
	argv = app.ensure_utf8(argv);
	// default config file name is config.toml in cwd
	static std::string config_file = "config.toml";
	app.add_option("-c,--config", config_file, "Config file path");
	static bool use_default = false;
	app.add_flag("--default", use_default, "Use default config");
	static bool use_debug = false;
	app.add_flag("-d,--debug", use_debug, "Enable debug log");
	static bool use_trace = false;
	app.add_flag("--trace", use_trace, "Enable trace log");
	CLI11_PARSE(app, argc, argv);
	if (use_debug) {
		spdlog::set_level(spdlog::level::debug);
	} else if (use_trace) {
		spdlog::set_level(spdlog::level::trace);
	} else {
		spdlog::set_level(spdlog::level::info);
	}

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
		spdlog::error("failed to parse config file: {}", e.what());
		return 1;
	}
	app::Config config;
	try {
		config = app::Config::from_toml(config_tbl);
	} catch (const app::invalid_argument &e) {
		spdlog::error("invalid config: {}", e.what());
		return 1;
	}

	// https://libzmq.readthedocs.io/en/latest/zmq_ipc.html
	// https://libzmq.readthedocs.io/en/latest/zmq_inproc.html
	zmq::context_t ctx;
	zmq::socket_t sock(ctx, zmq::socket_type::push);
	try {
		sock.bind(config.zmq_address);
	} catch (const zmq::error_t &e) {
		spdlog::error("failed to bind to ZMQ address: `{}`", e.what());
		return 1;
	}
	spdlog::info("bind to ZMQ address: `{}`", config.zmq_address);

	std::cout << "Config Used: " << config.to_toml() << std::endl;
	cv::VideoCapture cap;
	// https://gstreamer.freedesktop.org/documentation/shm/shmsink.html?gi-language=c
	if (std::holds_alternative<int>(config.pipeline)) {
		const auto index = std::get<int>(config.pipeline);
		spdlog::info("open video source index (int): {}", index);
		cap.open(index, config.api_preference);
	} else {
		const auto pipeline = std::get<std::string>(config.pipeline);
		spdlog::info("open video source pipeline (string): {}", pipeline);
		cap.open(pipeline, config.api_preference);
	}
	if (not cap.isOpened()) {
		spdlog::error("failed to open video source. check OpenCV VideoCapture API support if you're sure the source is correct.");
		std::cout << cv::getBuildInformation() << std::endl;
		return 1;
	}
	static size_t frame_count = 0;
	static std::atomic_bool is_running{true};
	constexpr auto sigint_handler = [](int) {
		spdlog::info("SIGINT received, stopping...");
		is_running.store(false, std::memory_order::relaxed);
	};
	std::signal(SIGINT, sigint_handler);

retry_shm:
	int shm_fd = shm_open(config.name.c_str(), O_CREAT | O_RDWR, 775);
	if (shm_fd == -1) {
		spdlog::error("failed to create shared memory `{}`. {} ({})", config.name, strerror(errno), errno);
		if (errno == EACCES || errno == EEXIST) {
			// `ipcrm -M <name>` could be used to remove the shared memory
			auto err = shm_unlink(config.name.c_str());
			if (err == -1) {
				spdlog::error("failed to unlink shared memory `{}`. {} ({})", config.name, strerror(errno), errno);
				return 1;
			} else {
				spdlog::warn("unlink shared memory `{}`", config.name);
				goto retry_shm;
			}
		}
		return 1;
	}
	// defer at exit
	const auto shm_close_fn = [shm_fd, name = config.name]() {
		auto err = close(shm_fd);
		if (err == -1) {
			spdlog::error("failed to close shared memory `{}`. reason: {}", name, strerror(errno));
			return err;
		}
		err = shm_unlink(name.c_str());
		if (err == -1) {
			spdlog::error("failed to unlink shared memory `{}`. reason: {}", name, strerror(errno));
			return err;
		}
		return 0;
	};

	cv::Mat frame;
	using start_ret_t         = std::tuple<void *, frame_info_t>;
	const auto at_first_frame = [shm_fd, &cap, &frame] -> std::expected<start_ret_t, int> {
		using ue_t = std::unexpected<int>;
		cap >> frame;
		if (frame.empty()) {
			spdlog::error("failed to capture first frame");
			return ue_t{-1};
		}
		frame_info_t info{
			.width      = static_cast<uint16_t>(frame.cols),
			.height     = static_cast<uint16_t>(frame.rows),
			.depth      = static_cast<uint8_t>(frame.depth()),
			.bufferSize = static_cast<uint32_t>(frame.total() * frame.elemSize()),
		};

		spdlog::info("first frame info: {}x{}x{}; depth={}({}); stride[0]={}; stride[1]={}; total={}; elemSize={}; bufferSize={}",
					 frame.cols,
					 frame.rows,
					 frame.channels(),
					 app::depth_to_string(frame.depth()),
					 frame.depth(),
					 frame.step[0],
					 frame.step[1],
					 frame.total(),
					 frame.elemSize(),
					 frame.total() * frame.elemSize());

		const auto size = frame.total() * frame.elemSize();
		auto ptr        = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (ptr == MAP_FAILED) {
			spdlog::error("failed to mmap shared memory. {}", strerror(errno));
			return ue_t{-1};
		}
		if (ftruncate(shm_fd, size) == -1) {
			spdlog::error("failed to truncate shared memory. {}", strerror(errno));
			return ue_t{-1};
		}
		memcpy(ptr, frame.data, size);
		return std::make_tuple(ptr, info);
	};

	start_ret_t tmp_ret;
	if (auto ret = at_first_frame(); ret) {
		tmp_ret = *ret;
	} else {
		return 1;
	}
	auto [ptr, info]     = tmp_ret;
	const auto unmap_ptr = [ptr, size = frame.total() * frame.elemSize()]() {
		auto err = munmap(ptr, size);
		if (err == -1) {
			spdlog::error("failed to unmap shared memory. reason: {}", strerror(errno));
			return err;
		}
		return 0;
	};

	const auto set_frame = [ptr, bufferSize = info.bufferSize](const cv::Mat &frame) {
		memcpy(ptr, frame.data, bufferSize);
	};

	while (is_running.load(std::memory_order::relaxed)) {
		cap >> frame;
		if (frame.empty()) {
			spdlog::error("failed to capture frame");
			break;
		} else {
			set_frame(frame);
			spdlog::debug("frame@{}", frame_count);
			try {
				const auto msg = sync_message_t{
					.frame_count = static_cast<uint32_t>(frame_count),
					.info        = info,
				};
				sock.send(zmq::buffer(reinterpret_cast<const uint8_t *>(&msg), sizeof(sync_message_t)), zmq::send_flags::dontwait);
			} catch (const zmq::error_t &e) {
				spdlog::error("failed to send synchronization message for frame@{}; {}", frame_count, e.what());
			}
		}
		frame_count += 1;
	}

	unmap_ptr();
	shm_close_fn();
	spdlog::info("normally exit");
	return 0;
}
