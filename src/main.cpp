#include <atomic>
#include <cstdint>
#include <iostream>
#include <filesystem>
#include <format>
#include <csignal>
#include <opencv2/core/hal/interface.h>
#include <unordered_map>
#include <string_view>
#include <string>
#include <regex>
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
#include "version/app_version.hpp"

#if defined(__APPLE__) && defined(__MACH__)
#define __APP_MACOS__
#endif
#ifdef __APP_MACOS__
// https://en.wikipedia.org/wiki/Unistd.h
#include <unistd.h>
#endif


namespace app {
/// @note use with `pixel_format` field in `frame_info_t`
enum class PixelFormat : uint8_t {
	/// usually 24bit RGB (8bit per channel, depth=U8)
	RGB = 0,
	BGR,
	RGBA,
	BGRA,
	/// channel=1
	GRAY,
	YUV,
	YUYV,
};

/// @note use with `depth` field in `frame_info_t`
enum class Depth : uint8_t {
	U8  = CV_8U,
	S8  = CV_8S,
	U16 = CV_16U,
	S16 = CV_16S,
	S32 = CV_32S,
	F32 = CV_32F,
	F64 = CV_64F,
	F16 = CV_16F,
};


using invalid_argument = std::invalid_argument;


constexpr auto FRAME_TOPIC_MAGIC = 0x7d;
using cap_api_t                  = decltype(cv::CAP_ANY);

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
	/// whether the video source is looped, when it's a finite source
	bool is_loop = false;

	static Config Default() {
		// https://github.com/opencv/opencv/blob/f503890c2b2ba73f4f94971c1845ead941143262/modules/videoio/src/cap_gstreamer.cpp#L1535
		// https://github.com/opencv/opencv/blob/f503890c2b2ba73f4f94971c1845ead941143262/modules/videoio/src/cap_gstreamer.cpp#L1503
		// an appsink called `opencvsink`
		return {
			.name           = "default",
			.pipeline       = "videotestsrc ! timeoverlay ! videoconvert ! video/x-raw,format=BGR ! appsink name=opencvsink",
			.api_preference = cv::CAP_GSTREAMER,
			.zmq_address    = "ipc:///tmp/0",
			.is_loop        = false,
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
		if (const auto is_loop = table["is_loop"]; is_loop) {
			config.is_loop = *is_loop.value<bool>();
		} else {
			config.is_loop = false;
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
			{"is_loop", is_loop},
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


const char *depth_to_string(const Depth depth) {
	switch (depth) {
	case Depth::U8:
		return "U8";
	case Depth::S8:
		return "S8";
	case Depth::U16:
		return "U16";
	case Depth::S16:
		return "S16";
	case Depth::F16:
		return "F16";
	case Depth::S32:
		return "S32";
	case Depth::F32:
		return "F32";
	case Depth::F64:
		return "F64";
	default:
		return "unknown";
	}
}

const char *cv_depth_to_string(const int depth) {
	return depth_to_string(static_cast<Depth>(depth));
}

const char *pixel_format_to_string(const PixelFormat fmt) {
	switch (fmt) {
	case PixelFormat::RGB:
		return "RGB";
	case PixelFormat::BGR:
		return "BGR";
	case PixelFormat::RGBA:
		return "RGBA";
	case PixelFormat::BGRA:
		return "BGRA";
	case PixelFormat::GRAY:
		return "GRAY";
	case PixelFormat::YUV:
		return "YUV";
	case PixelFormat::YUYV:
		return "YUYV";
	default:
		return "unknown";
	}
}

/// @brief convert color depth to size in bytes
/// @sa https://gist.github.com/yangcha/38f2fa630e223a8546f9b48ebbb3e61a
inline int depth_to_size(Depth depth) {
	switch (depth) {
	case Depth::U8:
	case Depth::S8:
		return 1;
	case Depth::U16:
	case Depth::S16:
	case Depth::F16:
		return 2;
	case Depth::S32:
	case Depth::F32:
		return 4;
	case Depth::F64:
		return 8;
	default:
		throw app::invalid_argument(std::format("invalid depth value `{}`", static_cast<int>(depth)));
	}
}

inline int cv_depth_to_size(int depth) {
	return depth_to_size(static_cast<Depth>(depth));
}


PixelFormat guess_pixel_format(const int channels) {
	switch (channels) {
	case 1:
		return PixelFormat::GRAY;
	case 3:
		return PixelFormat::BGR;
	case 4:
		return PixelFormat::BGRA;
	default:
		throw invalid_argument(std::format("invalid channel count: `{}`", channels));
	}
};


// https://docs.opencv.org/4.x/d3/d63/classcv_1_1Mat.html
// See `Detailed Description`
// strides for each dimension
// stride[0]=channel
// stride[1]=channel*cols
// stride[2]=channel*cols*rows
struct __attribute__((packed)) frame_info_t {
	uint16_t width;
	uint16_t height;
	uint8_t channels;
	/// CV_8U, CV_8S, CV_16U, CV_16S, CV_16F, CV_32S, CV_32F, CV_64F
	Depth depth;
	uint32_t buffer_size;
	PixelFormat pixel_format = PixelFormat::BGR;

	/// @brief pixel size in bytes
	[[nodiscard]]
	int pixelSize() const {
		return depth_to_size(depth) * channels;
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
	/// this field SHOULD NOT be modified
	uint8_t magic = FRAME_TOPIC_MAGIC;
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


int main(int argc, char **argv) {
	using namespace app;
	constexpr auto ipc_prefix = "ipc://";
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
	zmq::socket_t sock(ctx, zmq::socket_type::pub);
	const auto close_zmq = [&sock, &ctx, zmq_address = config.zmq_address] {
		sock.close();
		ctx.close();
		if (zmq_address.starts_with(ipc_prefix)) {
			const auto path = zmq_address.substr(std::string_view(ipc_prefix).size());
			const auto err  = unlink(path.c_str());
			if (err == -1) {
				spdlog::warn("failed to unlink ZMQ address `{}` because of `{} ()`", path, strerror(errno), errno);
			}
		}
	};
	try {
		// https://zguide.zeromq.org/docs/chapter2/
		// The inter-process ipc transport is disconnected, like tcp. It has one
		// limitation: it does not yet work on Windows. By convention we use
		// endpoint names with an “.ipc” extension to avoid potential conflict
		// with other file names. On UNIX systems, if you use ipc endpoints you
		// need to create these with appropriate permissions otherwise they may
		// not be shareable between processes running under different user IDs.
		// You must also make sure all processes can access the files, e.g., by
		// running in the same working directory.
		sock.bind(config.zmq_address);
		if (config.zmq_address.starts_with(ipc_prefix)) {
			const auto path         = config.zmq_address.substr(std::string_view(ipc_prefix).size());
			constexpr auto mode_777 = S_IRWXU | S_IRWXG | S_IRWXO;
			const auto ok           = chmod(path.c_str(), mode_777);
			if (ok == -1) {
				spdlog::warn("failed to chmod ZMQ address `{}` because of `{}`", path, strerror(errno));
			}
		}
	} catch (const zmq::error_t &e) {
		spdlog::error("failed to bind to ZMQ address: `{}`", e.what());
		return 1;
	}
	spdlog::info("bind to ZMQ address: `{}`", config.zmq_address);
	cv::VideoCapture cap;
	// https://gstreamer.freedesktop.org/documentation/shm/shmsink.html?gi-language=c
	if (std::holds_alternative<int>(config.pipeline)) {
		const auto index = std::get<int>(config.pipeline);
		spdlog::info("open video source index (int): {}", index);
		cap.open(index, config.api_preference);
	} else {
		const auto pipeline               = std::get<std::string>(config.pipeline);
		constexpr auto check_gst_pipeline = [](std::string pipeline) {
			std::regex re(R"(\,\s+)");
			std::smatch m;
			if (std::regex_search(pipeline, m, re)) {
				spdlog::warn("extra spaces found in the pipeline string: `{}`. "
							 "GStreamer won't happy about extra space in caps. "
							 "please remove them, otherwise it may cause unexpected behavior.",
							 pipeline);
			}
		};
		if (config.api_preference == cv::CAP_GSTREAMER) {
			check_gst_pipeline(pipeline);
		}
		spdlog::info("open video source pipeline (string): {}", pipeline);
		cap.open(pipeline, config.api_preference);
	}
	if (not cap.isOpened()) {
		spdlog::error("failed to open video source. check OpenCV VideoCapture API support if you're sure the source is correct.");
		std::cout << cv::getBuildInformation() << std::endl;
		return 1;
	}

	struct finite_source_info_t {
		double fps;
		uint32_t frame_count;
	};
	const auto check_finite_source = [&cap] -> std::optional<finite_source_info_t> {
		const auto fps         = cap.get(cv::CAP_PROP_FPS);
		const auto frame_count = cap.get(cv::CAP_PROP_FRAME_COUNT);
		if (fps > 0 and frame_count > 0) {
			return finite_source_info_t{
				.fps         = fps,
				.frame_count = static_cast<uint32_t>(frame_count),
			};
		}
		return std::nullopt;
	};

	const auto reset_video_position = [&cap] {
		cap.set(cv::CAP_PROP_POS_FRAMES, 0);
	};
	const auto get_video_position = [&cap] -> int {
		return static_cast<int>(cap.get(cv::CAP_PROP_POS_FRAMES));
	};


	static size_t frame_count = 0;
	static std::atomic_bool is_running{true};
	constexpr auto sigint_handler = [](int) {
		spdlog::info("SIGINT received, stopping...");
		is_running.store(false, std::memory_order::relaxed);
	};
	std::signal(SIGINT, sigint_handler);
	const auto finite_source_info = check_finite_source();
	const auto frame_interval_ms  = [finite_source_info] -> std::optional<int> {
        if (finite_source_info) {
            return static_cast<int>(1000.0 / finite_source_info->fps);
        } else {
            return std::nullopt;
        }
	}();
	if (finite_source_info) {
		spdlog::info("detected finite source; fps={} ({}ms), frame_count={}, is_loop={}",
					 finite_source_info->fps, *frame_interval_ms, finite_source_info->frame_count, config.is_loop);
	} else {
		spdlog::info("infinite source detected (live stream)");
	}

retry_shm:
	int shm_fd = shm_open(config.name.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
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
	spdlog::debug("created shared memory `{}` (fd={})", config.name, shm_fd);
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
		const auto pixel_format = guess_pixel_format(frame.channels());
		frame_info_t info{
			.width        = static_cast<uint16_t>(frame.cols),
			.height       = static_cast<uint16_t>(frame.rows),
			.channels     = static_cast<uint8_t>(frame.channels()),
			.depth        = static_cast<Depth>(frame.depth()),
			.buffer_size  = static_cast<uint32_t>(frame.total() * frame.elemSize()),
			.pixel_format = pixel_format,
		};

		spdlog::info("first frame info: {}x{}x{}; "
					 "depth={}({}); "
					 "stride[0]={}; "
					 "stride[1]={}; "
					 "total={}; "
					 "elemSize={}; "
					 "bufferSize={}; "
					 "pixelFormat={};",
					 frame.cols,
					 frame.rows,
					 frame.channels(),
					 app::cv_depth_to_string(frame.depth()),
					 frame.depth(),
					 frame.step[0],
					 frame.step[1],
					 frame.total(),
					 frame.elemSize(),
					 frame.total() * frame.elemSize(),
					 app::pixel_format_to_string(pixel_format));

		const auto size = frame.total() * frame.elemSize();
		// https://www.deepanseeralan.com/tech/playing-with-shared-memory/
		// ftruncate first, then mmap
		if (ftruncate(shm_fd, size) == -1) {
			spdlog::error("failed to truncate shared memory; {} ({})", strerror(errno), errno);
			return ue_t{-1};
		}
		auto ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (ptr == MAP_FAILED) {
			// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mmap.2.html
			spdlog::error("failed to mmap shared memory; {} ({})", strerror(errno), errno);
			return ue_t{-1};
		}
		memcpy(ptr, frame.data, size);
		return std::make_tuple(ptr, info);
	};

	start_ret_t tmp_ret;
	if (auto ret = at_first_frame(); ret) {
		tmp_ret = *ret;
	} else {
		shm_close_fn();
		close_zmq();
		return 1;
	}

	const auto [ptr, info] = tmp_ret;

	const auto unmap_ptr = [ptr, bufferSize = info.buffer_size] {
		int err;
		err = munmap(ptr, bufferSize);
		if (err == -1) {
			spdlog::error("failed to unmap shared memory. reason: {}", strerror(errno));
			return err;
		}
		return 0;
	};

	const auto set_frame = [ptr, bufferSize = info.buffer_size](const cv::Mat &frame) {
		// TODO: check frame size
		memcpy(ptr, frame.data, bufferSize);
	};

	const auto send_sync_msg = [&sock, &info] {
		try {
			const auto msg = sync_message_t{
				.frame_count = static_cast<uint32_t>(frame_count),
				.info        = info,
			};
			sock.send(zmq::buffer(reinterpret_cast<const uint8_t *>(&msg), sizeof(sync_message_t)), zmq::send_flags::none);
		} catch (const zmq::error_t &e) {
			spdlog::error("failed to send synchronization message for frame@{}; {}", frame_count, e.what());
		}
	};

	send_sync_msg();
	while (is_running.load(std::memory_order::relaxed)) {
		cap >> frame;
		if (frame.empty()) {
			if (finite_source_info) {
				spdlog::info("reached end of finite video source");
				if (config.is_loop) {
					reset_video_position();
				} else {
					break;
				}
			} else {
				spdlog::warn("live source empty frame captured");
				break;
			}
		} else {
			set_frame(frame);
			send_sync_msg();
			if (finite_source_info) {
				const auto current = get_video_position();
				spdlog::debug("frame@{} ({}/{})", frame_count, current, finite_source_info->frame_count);
				std::this_thread::sleep_for(std::chrono::milliseconds(*frame_interval_ms));
			} else {
				spdlog::debug("frame@{}", frame_count);
			}
		}
		frame_count += 1;
	}

	unmap_ptr();
	shm_close_fn();
	close_zmq();
	spdlog::info("normally exit");
	return 0;
}
