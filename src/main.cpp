#include <atomic>
#include <cstdint>
#include <filesystem>
#include <csignal>
#include <string_view>
#include <string>
#include <expected>
#include <span>
#include <thread>
#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <zmq.hpp>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "version/app_version.hpp"
#include "config/app_config.hpp"
#include "models/app_metadata_models.hpp"
#include "app_utils.hpp"
#include "backends/app_backends_facade.hpp"
#ifdef WITH_BACKEND_OPENCV
#include "backends/app_backends_opencv.hpp"
#endif
#ifdef WITH_BACKEND_GSTREAMER
#include "backends/app_backends_gst.hpp"
#endif

#if defined(__APPLE__) && defined(__MACH__)
#define __APP_MACOS__
#endif
#ifdef __APP_MACOS__
// https://en.wikipedia.org/wiki/Unistd.h
#include <unistd.h>
#endif

// APP_DEBUG_SYNC_MESSAGE_DUMP

int main(int argc, char **argv) {
	using namespace app;
	constexpr auto IPC_PREFIX = "ipc://";
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
		spdlog::error("loading config: {}", e.what());
		return 1;
	}

	// https://libzmq.readthedocs.io/en/latest/zmq_ipc.html
	// https://libzmq.readthedocs.io/en/latest/zmq_inproc.html
	// note that `zmq::socket_t` is RAII aware already
	zmq::context_t ctx;
	zmq::socket_t sock(ctx, zmq::socket_type::pub);
	deferrer zmq_deferrer([&sock, &ctx, zmq_address = config.zmq_address()] {
		if (zmq_address.starts_with(IPC_PREFIX)) {
			const auto path = zmq_address.substr(std::string_view(IPC_PREFIX).size());
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
		if (config.zmq_address().starts_with(IPC_PREFIX)) {
			const auto path = config.zmq_address().substr(std::string_view(IPC_PREFIX).size());
			// 777
			const auto ok = chmod(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
			if (ok == -1) {
				spdlog::warn("chmod ZMQ address `{}` because of `{}`", path, strerror(errno));
			}
		}
	} catch (const zmq::error_t &e) {
		spdlog::error("bind to ZMQ address: `{}`", e.what());
		return 1;
	}
	spdlog::info("bond to ZMQ address: `{}`", config.zmq_address());

	static auto is_running = std::atomic_bool{true};

	/**
	 * @brief signal handler for SIGINT
	 */
	constexpr auto sigint_handler = [](int) {
		spdlog::info("SIGINT received, stopping...");
		is_running.store(false, std::memory_order::relaxed);
	};
	std::signal(SIGINT, sigint_handler);

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
			// mode=0666
			// shouldn't be 777. It's generally not needed unless you're putting
			// an ELF binary in the shared memory.
			// which is not the case here.
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
		spdlog::error("opening shared memory `{}`. reason: {}", config.shm_name(), shm_state_.error());
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
			metadata().ensure_magic();
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
				return ue_t{errno};
			}
			auto ptr = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
			if (ptr == MAP_FAILED) {
				// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mmap.2.html
				spdlog::error("mmap shared memory; fd={}, errno={} ({})", shm_fd, errno, strerror(errno));
				return ue_t{errno};
			}
			return frame_state_t(std::span<uint8_t>(ptr, size));
		}

		void set_frame_count(uint32_t frame_count) {
			metadata().frame_count_atomic().store(frame_count, std::memory_order::relaxed);
		}

		void set_timestamp_ns(uint64_t timestamp_ns) {
			metadata().timestamp_ns_atomic().store(timestamp_ns, std::memory_order::relaxed);
		}


	private:
		uint8_t *_mmap_ptr;
		/** [0, SHM_PAYLOAD_OFFSET) (metadata) */
		std::span<uint8_t> _metadata_buffer;
		/** [SHM_PAYLOAD_OFFSET, total_buffer_size) */
		std::span<uint8_t> _image_buffer;
	};

	// Frame state will be initialized by on_metadata callback
	std::optional<frame_state_t> frame_state;
	std::optional<sync_message_t> sync_msg;

	// Create backend based on config
	pro::proxy<app::backends::IBackend> backend;
	switch (config.video.backend) {
#ifdef WITH_BACKEND_OPENCV
	case app::BackendType::OpenCV: {
		if (!config.opencv) {
			spdlog::error("OpenCV backend selected but [opencv] config section missing");
			return 1;
		}
		backend = pro::make_proxy<app::backends::IBackend, app::backends::OpenCVBackend>(
			config.opencv->parameter,
			config.video,
			config.opencv->api_preference);
		spdlog::info("using OpenCV backend");
		break;
	}
#endif
#ifdef WITH_BACKEND_GSTREAMER
	case app::BackendType::GStreamer: {
		if (!config.gstreamer) {
			spdlog::error("GStreamer backend selected but [gstreamer] config section missing");
			return 1;
		}
		backend = pro::make_proxy<app::backends::IBackend, app::backends::GStreamerBackend>(
			config.gstreamer->pipeline,
			config.video);
		spdlog::info("using GStreamer backend");
		break;
	}
#endif
	default:
		spdlog::error("selected backend is not available in this build");
		return 1;
	}

	backend->SetOnMetadata([&shm_state, &frame_state, &sync_msg, &config](const frame_metadata_t &metadata) {
		const auto picture_buffer_size = metadata.info.buffer_size;
		const auto total_buffer_size   = SHM_PAYLOAD_OFFSET + picture_buffer_size;

		auto fs = frame_state_t::open(shm_state.fd(), total_buffer_size);
		if (not fs) {
			spdlog::error("open frame state; {}", fs.error());
			return;
		}
		fs->metadata().frame_count_atomic().store(0, std::memory_order::relaxed);
		fs->metadata().timestamp_ns_atomic().store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count(), std::memory_order::relaxed);
		fs->metadata().info = metadata.info;
		frame_state         = std::move(*fs);
		sync_msg.emplace(config.name, 0);
	});

	backend->SetOnFrame([&frame_state, &sync_msg, &sock](std::span<uint8_t> frame_buffer, const frame_metadata_t &metadata) {
		if (not frame_state || not sync_msg) {
			spdlog::error("[BUG] frame callback invoked before metadata callback (should not happen)");
			return;
		}

		// Copy frame data to shared memory
		auto &fs                       = *frame_state;
		const auto picture_buffer_size = frame_buffer.size();
		assert(picture_buffer_size == fs.image_buffer().size());
		std::copy(frame_buffer.begin(), frame_buffer.end(), fs.image_buffer().begin());
		fs.set_frame_count(metadata.frame_count);
		fs.set_timestamp_ns(metadata.timestamp_ns);

		// Send sync message
		try {
			std::array<uint8_t, sync_message_t::size()> buffer;
			sync_msg->set_frame_count(metadata.frame_count);
			sync_msg->set_timestamp_ns(metadata.timestamp_ns);
			std::copy(
				sync_msg->as_uint8s().begin(),
				sync_msg->as_uint8s().end(),
				buffer.begin());
#ifdef APP_DEBUG_SYNC_MESSAGE_DUMP
			spdlog::debug("sync_msg hex dump:\n{}", hexdump(buffer));
#endif
			sock.send(zmq::buffer(buffer), zmq::send_flags::none);
		} catch (const zmq::error_t &e) {
			spdlog::error("send synchronization message for frame@{}; {}", metadata.frame_count, e.what());
		}
	});

	const auto send_status = [&sock, name = config.name](int32_t status) {
		try {
			std::array<uint8_t, module_status_message_t::size()> buffer;
			auto msg = module_status_message_t{};
			msg._fill_with_status(status, name);
			std::copy(
				msg.as_uint8s().begin(),
				msg.as_uint8s().end(),
				buffer.begin());
			sock.send(zmq::buffer(buffer), zmq::send_flags::none);
		} catch (const zmq::error_t &e) {
			spdlog::error("send module status message; {}", e.what());
		}
	};

	backend->SetOnError([&backend, &config, send_status](int error_code, std::string_view message) {
		if (error_code == backends::ERR_EOS) {
			spdlog::info("backend EOF: {}", message);
			if (config.video.finite_stream_ending_behavior == app::FiniteStreamEndingBehavior::Loop) {
				spdlog::info("looping finite stream (encore)");
				auto err = backend->ResetFrameCount();
				if (err != backends::ERR_OK) {
					spdlog::error("resetting frame count for loop: {}", err);
					is_running.store(false, std::memory_order::relaxed);
				} else {
					send_status(MODULE_STATUS_STREAM_RESET);
				}
				return;
			}
		} else {
			spdlog::error("backend({}): {}", error_code, message);
		}
		// stop the looping on any error (or non-loop EOF)
		is_running.store(false, std::memory_order::relaxed);
	});

	backend->Init();
	send_status(MODULE_STATUS_ONLINE);

	// Wait for shutdown signal
	while (is_running.load(std::memory_order::relaxed)) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	backend->Shutdown();
	send_status(MODULE_STATUS_OFFLINE);

	spdlog::info("normally exit");
	return 0;
}
