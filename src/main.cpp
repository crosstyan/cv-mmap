#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <filesystem>
#include <format>
#include <csignal>
#include <string_view>
#include <string>
#include <regex>
#include <expected>
#include <span>
#include <CLI/CLI.hpp>
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
#include "config/app_config.hpp"
#include <app_models.hpp>
#include <stdexcept>

#if defined(__APPLE__) && defined(__MACH__)
#define __APP_MACOS__
#endif
#ifdef __APP_MACOS__
// https://en.wikipedia.org/wiki/Unistd.h
#include <unistd.h>
#endif

constexpr auto NAME_MAX_LEN = 24;
/**
 * @brief offset of the shared memory payload
 * @note the first 256 bytes are reserved for the frame info and other useful metadata
 */
constexpr auto SHM_PAYLOAD_OFFSET = 256;

namespace app {
using invalid_argument           = std::invalid_argument;
constexpr auto FRAME_TOPIC_MAGIC = 0x7d;
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
	/** properties */
	uint16_t width;
	uint16_t height;
	uint8_t channels;
	/// CV_8U, CV_8S, CV_16U, CV_16S, CV_16F, CV_32S, CV_32F, CV_64F
	Depth depth;
	uint32_t buffer_size;
	PixelFormat pixel_format = PixelFormat::BGR;
	/** end of properties */

	/// @brief pixel size in bytes
	[[nodiscard]]
	int pixelSize() const {
		return size_of(depth) * channels;
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
public:
	static constexpr auto LABEL_LEN_MAX = NAME_MAX_LEN;
	using label                         = uint8_t[NAME_MAX_LEN];
	struct __attribute__((packed)) attr {
		uint32_t frame_count;
	};

	sync_message_t(const std::string_view &label, uint32_t frame_count) : _attribute{.frame_count = frame_count} {
		if (label.size() > LABEL_LEN_MAX) {
			throw invalid_argument(std::format("label is too long: `{}`", label));
		}
		std::copy(label.begin(), label.end(), _label);
		std::fill(_label + label.size(), _label + LABEL_LEN_MAX, '\0');
	}

	sync_message_t &set_frame_count(uint32_t frame_count) {
		_attribute.frame_count = frame_count;
		return *this;
	}

	static constexpr size_t size() {
		return sizeof(_magic) + sizeof(sync_message_t);
	}

	/**
	 * @brief marshal the sync message to the buffer
	 * @param buf the buffer to marshal the sync message to
	 * @return the size of the marshaled sync message
	 * @note the buffer size must be at least `size()`
	 */
	int marshal(std::span<uint8_t> buf) const {
		if (buf.size() < size()) {
			return -1;
		}
		uint8_t *ptr    = buf.data();
		*ptr++          = _magic;
		const auto self = std::span<const uint8_t>{
			reinterpret_cast<const uint8_t *>(this), sizeof(sync_message_t)};
		std::copy(self.begin(), self.end(), ptr);
		return size();
	}

private:
	static constexpr uint8_t _magic = FRAME_TOPIC_MAGIC;
	/**
	 * @brief label of the video source
	 * @note C string, null-terminated
	 */
	attr _attribute;
	label _label;
};

struct __attribute__((packed)) frame_metadata_t {
	static constexpr auto CV_MMAP_MAGIC =
		std::array<char, 8>{'C', 'V', '-', 'M', 'M', 'A', 'P', '\0'};

	int marshal(std::span<uint8_t> buf) const {
		// the first 8 bytes should be CV_MMAP_MAGIC
		constexpr auto REQUIRED_SIZE = CV_MMAP_MAGIC.size() + sizeof(frame_info_t);
		if (buf.size() < REQUIRED_SIZE) {
			return -1;
		}
		std::copy(CV_MMAP_MAGIC.begin(), CV_MMAP_MAGIC.end(), buf.begin());
		std::copy(
			reinterpret_cast<const uint8_t *>(&info), reinterpret_cast<const uint8_t *>(&info) + sizeof(frame_info_t), buf.begin() + CV_MMAP_MAGIC.size());
		return CV_MMAP_MAGIC.size() + sizeof(frame_info_t);
	}

	/** properties */
	uint32_t frame_count;
	frame_info_t info;
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
	if (use_trace) {
		spdlog::set_level(spdlog::level::trace);
	} else if (use_debug) {
		spdlog::set_level(spdlog::level::debug);
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

	app::Config config;
	try {
		config = app::Config::from_toml(config_path);
	} catch (const std::exception &e) {
		spdlog::error("Failed to load config: {}", e.what());
		return 1;
	}

	/**
	 * @brief a simple RAII helper for `defer` like behavior
	 */
	struct deferrer {
		deferrer(std::move_only_function<void()> &&f) : _f(std::move(f)) {}
		~deferrer() {
			if (_f) {
				_f();
			}
		}
		deferrer(const deferrer &)            = delete;
		deferrer &operator=(const deferrer &) = delete;
		deferrer(deferrer &&other) noexcept : _f(std::move(other._f)) {
			other._f = {};
		}
		deferrer &operator=(deferrer &&other) noexcept {
			if (this != &other) {
				// call current function before overwriting
				if (_f) {
					_f();
				}
				_f       = std::move(other._f);
				other._f = {};
			}
			return *this;
		}

	private:
		std::move_only_function<void()> _f{};
	};

	// https://libzmq.readthedocs.io/en/latest/zmq_ipc.html
	// https://libzmq.readthedocs.io/en/latest/zmq_inproc.html
	// note that `zmq::socket_t` is RAII aware already
	zmq::context_t ctx;
	zmq::socket_t sock(ctx, zmq::socket_type::pub);
	deferrer zmq_deferrer([&sock, &ctx, zmq_address = config.zmq_address()] {
		if (zmq_address.starts_with(ipc_prefix)) {
			const auto path = zmq_address.substr(std::string_view(ipc_prefix).size());
			const auto err  = unlink(path.c_str());
			if (err == -1) {
				spdlog::error("unlink ZMQ address `{}` because of `{} ({})`", path, strerror(errno), errno);
			}
		}
	});

	try {
		// https://zguide.zeromq.org/docs/chapter2/
		// The inter-process ipc transport is disconnected, like tcp. It has one
		// limitation: it does not yet work on Windows. By convention we use
		// endpoint names with an "extension to avoid potential conflict
		// with other file names. On UNIX systems, if you use ipc endpoints you
		// need to create these with appropriate permissions otherwise they may
		// not be shareable between processes running under different user IDs.
		// You must also make sure all processes can access the files, e.g., by
		// running in the same working directory.
		sock.bind(config.zmq_address());
		if (config.zmq_address().starts_with(ipc_prefix)) {
			const auto path         = config.zmq_address().substr(std::string_view(ipc_prefix).size());
			constexpr auto mode_777 = S_IRWXU | S_IRWXG | S_IRWXO;
			const auto ok           = chmod(path.c_str(), mode_777);
			if (ok == -1) {
				spdlog::warn("chmod ZMQ address `{}` because of `{}`", path, strerror(errno));
			}
		}
	} catch (const zmq::error_t &e) {
		spdlog::error("bind to ZMQ address: `{}`", e.what());
		return 1;
	}
	spdlog::info("bind to ZMQ address: `{}`", config.zmq_address());
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
		if (static_cast<cv::VideoCaptureAPIs>(config.api_preference) == cv::CAP_GSTREAMER) {
			check_gst_pipeline(pipeline);
		}
		spdlog::info("open video source pipeline (string): {}", pipeline);
		cap.open(pipeline, config.api_preference);
	}
	if (not cap.isOpened()) {
		spdlog::error("open video source. check OpenCV VideoCapture API support if you're sure the source is correct.");
		std::cout << cv::getBuildInformation() << std::endl;
		return 1;
	}

	struct finite_source_info_t {
		double fps;
		uint32_t frame_count;

		std::chrono::milliseconds frame_interval() const {
			return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(1000.0 / fps));
		}
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


	static uint32_t frame_count = 0;
	static auto is_running      = std::atomic_bool{true};

	/**
	 * @brief signal handler for SIGINT
	 */
	constexpr auto sigint_handler = [](int) {
		spdlog::info("SIGINT received, stopping...");
		is_running.store(false, std::memory_order::relaxed);
	};
	std::signal(SIGINT, sigint_handler);

	const auto finite_source_info = check_finite_source();
	if (finite_source_info) {
		spdlog::info("detected finite source; fps={} ({}ms), frame_count={}, is_loop={}",
					 finite_source_info->fps, finite_source_info->frame_interval().count(), finite_source_info->frame_count, config.is_loop);
	} else {
		spdlog::info("infinite source detected (live stream)");
	}

	/**
	 * @brief a simple RAII wrapper for shared memory
	 */
	struct shm_state_t {
		shm_state_t(const std::string &name, int shm_fd) : _name(name), _shm_fd(shm_fd) {}
		~shm_state_t() {
			if (_shm_fd != -1) {
				spdlog::debug("closing shared memory `{}` (fd={})", _name, _shm_fd);
				close(_shm_fd);
				shm_unlink(_name.c_str());
			}
		}
		shm_state_t(const shm_state_t &)            = delete;
		shm_state_t &operator=(const shm_state_t &) = delete;
		shm_state_t(shm_state_t &&other) noexcept : _name(std::move(other._name)), _shm_fd(other._shm_fd) {
			other._shm_fd = -1;
		}
		shm_state_t &operator=(shm_state_t &&other) noexcept {
			if (this != &other) {
				// close current if valid
				if (_shm_fd != -1) {
					close(_shm_fd);
					shm_unlink(_name.c_str());
				}
				_name         = std::move(other._name);
				_shm_fd       = other._shm_fd;
				other._shm_fd = -1;
			}
			return *this;
		}

		static std::expected<shm_state_t, int> open(const std::string &name) {
			using ue_t = std::unexpected<int>;
			spdlog::debug("opening shared memory `{}`", name);
			int shm_fd = shm_open(name.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
			if (shm_fd == -1) {
				// `ipcrm -M <name>` could be used to remove the shared memory as well
				if (errno == EACCES || errno == EEXIST) {
					auto err = shm_unlink(name.c_str());
					if (err == -1) {
						spdlog::error("unlinking shared memory `{}`. reason: {}", name, strerror(errno));
						return ue_t{errno};
					} else {
						spdlog::warn("unlinked shared memory `{}`", name);
						return shm_state_t::open(name);
					}
				}
				return ue_t{errno};
			}
			spdlog::debug("opened shared memory `{}` (fd={})", name, shm_fd);
			return shm_state_t(name, shm_fd);
		}

		int fd() const {
			return _shm_fd;
		}

		const std::string_view name() const {
			return _name;
		}

	private:
		std::string _name;
		int _shm_fd;
	};

	auto shm_state_ = shm_state_t::open(config.shm_name());
	if (not shm_state_) {
		spdlog::error("failed to open shared memory `{}`. reason: {}", config.shm_name(), shm_state_.error());
		return 1;
	}
	auto shm_state = std::move(*shm_state_);
	spdlog::debug("created shared memory `{}` (fd={})", config.shm_name(), shm_state.fd());

	/**
	 * @brief a simple RAII wrapper for memory mapped frame state
	 */
	struct frame_state_t {
		frame_state_t(std::span<uint8_t> buf) : _mmap_ptr(buf.data()),
												_metadata_buffer(buf.subspan(0, SHM_PAYLOAD_OFFSET)),
												_image_buffer(buf.subspan(SHM_PAYLOAD_OFFSET, buf.size() - SHM_PAYLOAD_OFFSET)) {
			assert(total_buffer_size() == buf.size());
		}
		~frame_state_t() {
			if (_mmap_ptr) {
				spdlog::debug("closing frame state (mmap_ptr={})", static_cast<void *>(_mmap_ptr));
				munmap(_mmap_ptr, total_buffer_size());
			}
		}
		frame_state_t(const frame_state_t &)            = delete;
		frame_state_t &operator=(const frame_state_t &) = delete;
		frame_state_t(frame_state_t &&other) noexcept : _mmap_ptr(other._mmap_ptr), _metadata_buffer(other._metadata_buffer), _image_buffer(other._image_buffer) {
			other._mmap_ptr        = {};
			other._metadata_buffer = {};
			other._image_buffer    = {};
		}
		frame_state_t &operator=(frame_state_t &&other) noexcept {
			if (this != &other) {
				if (_mmap_ptr) {
					munmap(_mmap_ptr, total_buffer_size());
				}
				_mmap_ptr              = other._mmap_ptr;
				_metadata_buffer       = other._metadata_buffer;
				_image_buffer          = other._image_buffer;
				other._mmap_ptr        = {};
				other._metadata_buffer = {};
				other._image_buffer    = {};
			}
			return *this;
		}

		const std::span<uint8_t> metadata_buffer() const {
			return _metadata_buffer;
		}
		const std::span<uint8_t> image_buffer() const {
			return _image_buffer;
		}

		size_t total_buffer_size() const {
			return _metadata_buffer.size() + _image_buffer.size();
		}

		frame_metadata_t &metadata() {
			return *reinterpret_cast<frame_metadata_t *>(_metadata_buffer.data());
		}

		static std::expected<frame_state_t, int> open(int shm_fd, size_t size) {
			using ue_t = std::unexpected<int>;
			// https://www.deepanseeralan.com/tech/playing-with-shared-memory/
			// ftruncate first, then mmap
			if (ftruncate(shm_fd, size) == -1) {
				spdlog::error("truncate shared memory; fd={}, errno={} ({})", shm_fd, errno, strerror(errno));
				return ue_t{-1};
			}
			auto ptr = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
			if (ptr == MAP_FAILED) {
				// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mmap.2.html
				spdlog::error("mmap shared memory; fd={}, errno={} ({})", shm_fd, errno, strerror(errno));
				return ue_t{-1};
			}
			return frame_state_t(std::span<uint8_t>(ptr, size));
		}

		/**
		 * @brief set the frame to the image buffer
		 * @note the frame size must be the same as the image buffer size
		 */
		void set_frame(const cv::Mat &frame) {
			const auto picture_buffer_size = frame.total() * frame.elemSize();
			assert(picture_buffer_size == _image_buffer.size());
			std::copy(frame.data, frame.data + picture_buffer_size, _image_buffer.begin());
		}

		void set_frame_count(uint32_t frame_count) {
			metadata().frame_count = frame_count;
		}


	private:
		uint8_t *_mmap_ptr;
		/** 0-SHM_PAYLOAD_OFFSET (metadata) */
		std::span<uint8_t> _metadata_buffer;
		/** SHM_PAYLOAD_OFFSET-total_buffer_size */
		std::span<uint8_t> _image_buffer;
	};

	cv::Mat frame;
	const auto on_initial_frame = [&shm_state, &cap, &frame] -> std::expected<frame_state_t, int> {
		using ue_t = std::unexpected<int>;
		cap >> frame;
		if (frame.empty()) {
			spdlog::error("capture first frame");
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

		spdlog::info("initial frame info: {}x{}x{}; "
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
					 app::to_str(static_cast<app::Depth>(frame.depth())),
					 frame.depth(),
					 frame.step[0],
					 frame.step[1],
					 frame.total(),
					 frame.elemSize(),
					 frame.total() * frame.elemSize(),
					 app::to_str(pixel_format));

		const auto picture_buffer_size = frame.total() * frame.elemSize();
		const auto total_buffer_size   = SHM_PAYLOAD_OFFSET + picture_buffer_size;

		auto frame_state = frame_state_t::open(shm_state.fd(), total_buffer_size);
		if (not frame_state) {
			spdlog::error("failed to open frame state; {}", frame_state.error());
			return ue_t{frame_state.error()};
		}
		frame_state->metadata().frame_count = 0;
		frame_state->metadata().info        = info;
		return frame_state;
	};

	auto frame_state_ = on_initial_frame();
	if (not frame_state_) {
		spdlog::error("failed to open frame state; {}", frame_state_.error());
		return 1;
	}
	auto frame_state         = std::move(*frame_state_);
	auto sync_msg            = sync_message_t(config.name, frame_count);
	const auto send_sync_msg = [&sync_msg, &sock] {
		try {
			sync_msg.set_frame_count(frame_count);
			std::array<uint8_t, sync_message_t::size()> buffer;
			const auto _ret = sync_msg.marshal(buffer);
			assert(_ret != -1);
			sock.send(zmq::buffer(buffer), zmq::send_flags::none);
		} catch (const zmq::error_t &e) {
			spdlog::error("send synchronization message for frame@{}; {}", frame_count, e.what());
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
			frame_state.set_frame(frame);
			frame_state.set_frame_count(frame_count);
			send_sync_msg();
			if (finite_source_info) {
				const auto current = get_video_position();
				spdlog::debug("frame@{} ({}/{})", frame_count, current, finite_source_info->frame_count);
				std::this_thread::sleep_for(finite_source_info->frame_interval());
			} else {
				spdlog::debug("frame@{}", frame_count);
			}
		}
		frame_count += 1;
	}

	spdlog::info("normally exit");
	return 0;
}
