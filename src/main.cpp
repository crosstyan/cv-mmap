#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <csignal>
#include <limits>
#include <optional>
#include <string_view>
#include <string>
#include <expected>
#include <vector>
#include <span>
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
#include "models/app_control_msg_models.hpp"
#include "app_utils.hpp"
#include "backends/app_backends_facade.hpp"
#include "backends/app_backends_dummy.hpp"
#include "app_preprocess_undistort.hpp"
#ifdef WITH_BACKEND_OPENCV
#include "backends/app_backends_opencv.hpp"
#endif
#ifdef WITH_BACKEND_GSTREAMER
#include "backends/app_backends_gst.hpp"
#endif
#ifdef WITH_BACKEND_ZED
#include "backends/app_backends_zed.hpp"
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

	const bool body_stream_enabled =
		config.video.backend == app::BackendType::ZED &&
		config.zed &&
		config.zed->body_tracking &&
		config.zed->body_tracking->enabled;

	// https://libzmq.readthedocs.io/en/latest/zmq_ipc.html
	// https://libzmq.readthedocs.io/en/latest/zmq_inproc.html
	// note that `zmq::socket_t` is RAII aware already
	zmq::context_t ctx;
	zmq::socket_t sock(ctx, zmq::socket_type::pub);
	std::optional<zmq::socket_t> body_sock;
	if (body_stream_enabled) {
		body_sock.emplace(ctx, zmq::socket_type::pub);
	}
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

	deferrer zmq_body_deferrer([&body_sock, zmq_body_address = config.zmq_body_address()] {
		if (!body_sock) {
			return;
		}
		if (zmq_body_address.starts_with(IPC_PREFIX)) {
			const auto path = zmq_body_address.substr(std::string_view(IPC_PREFIX).size());
			const auto err  = unlink(path.c_str());
			if (err == -1) {
				spdlog::error("unlink ZMQ body address `{}` because of `{} ({})`", path, strerror(errno), errno);
			}
		}
	});

	if (body_stream_enabled) {
		try {
			body_sock->bind(config.zmq_body_address());
			if (config.zmq_body_address().starts_with(IPC_PREFIX)) {
				const auto path = config.zmq_body_address().substr(std::string_view(IPC_PREFIX).size());
				const auto ok = chmod(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
				if (ok == -1) {
					spdlog::warn("chmod ZMQ body address `{}` because of `{}`", path, strerror(errno));
				}
			}
		} catch (const zmq::error_t &e) {
			spdlog::error("bind to ZMQ body address: `{}`", e.what());
			return 1;
		}
		spdlog::info("bond to ZMQ body address: `{}`", config.zmq_body_address());
	}

	// Control socket (REQ/REP pattern)
	zmq::socket_t control_sock(ctx, zmq::socket_type::rep);
	deferrer zmq_control_deferrer([&control_sock, zmq_control_address = config.zmq_control_address()] {
		if (zmq_control_address.starts_with(IPC_PREFIX)) {
			const auto path = zmq_control_address.substr(std::string_view(IPC_PREFIX).size());
			const auto err  = unlink(path.c_str());
			if (err == -1) {
				spdlog::error("unlink ZMQ control address `{}` because of `{} ({})`", path, strerror(errno), errno);
			}
		}
	});

	try {
		control_sock.bind(config.zmq_control_address());
		if (config.zmq_control_address().starts_with(IPC_PREFIX)) {
			const auto path = config.zmq_control_address().substr(std::string_view(IPC_PREFIX).size());
			const auto ok   = chmod(path.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
			if (ok == -1) {
				spdlog::warn("chmod ZMQ control address `{}` because of `{}`", path, strerror(errno));
			}
		}
		// Set receive timeout for non-blocking polling behavior
		control_sock.set(zmq::sockopt::rcvtimeo, 100); // 100ms timeout
	} catch (const zmq::error_t &e) {
		spdlog::error("bind to ZMQ control address: `{}`", e.what());
		return 1;
	}
	spdlog::info("bond to ZMQ control address: `{}`", config.zmq_control_address());

	static auto is_running   = std::atomic_bool{true};
	static auto sigint_count = std::atomic_int{0};

	/**
	 * @brief signal handler for SIGINT
	 */
	constexpr auto sigint_handler = [](int) {
		if (sigint_count.fetch_add(1, std::memory_order::relaxed) > 0) {
			spdlog::critical("SIGINT received twice, force killing...");
			std::exit(1);
		}
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
			std::fill(_metadata_buffer.begin(), _metadata_buffer.end(), 0);
			metadata().header.ensure_magic();
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

		frame_metadata_v2_t &metadata() {
			return *reinterpret_cast<frame_metadata_v2_t *>(_metadata_buffer.data());
		}

		void write_metadata(const frame_metadata_v2_t &metadata) {
			std::memcpy(_metadata_buffer.data(), &metadata, sizeof(metadata));
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
	auto undistort_pass = app::preprocess::make_undistort_pass(config.preprocess);
	if (undistort_pass) {
		spdlog::info("undistort preprocess pass is enabled");
	}

	const auto to_u32 = [](size_t value) -> std::optional<uint32_t> {
		if (value > std::numeric_limits<uint32_t>::max()) {
			return std::nullopt;
		}
		return static_cast<uint32_t>(value);
	};

	const auto now_ns = []() -> uint64_t {
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now().time_since_epoch())
			.count());
	};

	const auto serialize_body_tracking_frame = [&config](const cvmmap::body_tracking_frame_t &frame) -> std::vector<uint8_t> {
		auto header = frame.header;
		header._magic = cvmmap::BODY_TRACKING_MAGIC;
		header.versions_major = VERSION_MAJOR;
		header.versions_minor = VERSION_MINOR;
		std::memset(header._label, 0, sizeof(header._label));
		std::memcpy(
			header._label,
			config.name.data(),
			std::min(sizeof(header._label), config.name.size()));
		header.body_count = static_cast<uint16_t>(frame.bodies.size());
		header.body_record_size = sizeof(cvmmap::body_tracking_body_t);
		header.payload_size_bytes = static_cast<uint32_t>(
			frame.bodies.size() * sizeof(cvmmap::body_tracking_body_t));

		std::vector<uint8_t> bytes(
			sizeof(cvmmap::body_tracking_message_header_t) + header.payload_size_bytes);
		std::memcpy(bytes.data(), &header, sizeof(header));
		if (!frame.bodies.empty()) {
			std::memcpy(
				bytes.data() + sizeof(header),
				frame.bodies.data(),
				header.payload_size_bytes);
		}
		return bytes;
	};

	const auto determine_depth_unit = [&config]() -> DepthUnit {
		if (config.video.backend != app::BackendType::ZED || !config.zed.has_value()) {
			return DepthUnit::Unknown;
		}
		const auto &zed = *config.zed;
		const bool body_tracking_uses_meters =
			zed.body_tracking.has_value() && zed.body_tracking->enabled;
		return body_tracking_uses_meters ? DepthUnit::Meter : DepthUnit::Millimeter;
	};

	const auto build_v2_metadata = [&to_u32, &determine_depth_unit](const frame_metadata_t &source_metadata, size_t payload_size) -> std::optional<frame_metadata_v2_t> {
		if (payload_size == 0 || source_metadata.info.width == 0 || source_metadata.info.height == 0 || source_metadata.info.channels == 0) {
			return std::nullopt;
		}

		auto payload_size_u32 = to_u32(payload_size);
		if (!payload_size_u32) {
			return std::nullopt;
		}

		auto make_stride = [&to_u32](size_t plane_size, uint32_t height, size_t expected_min_stride) -> std::optional<uint32_t> {
			if (height == 0) {
				return std::nullopt;
			}
			size_t stride = expected_min_stride;
			if (plane_size % height == 0) {
				stride = std::max(expected_min_stride, plane_size / height);
			}
			return to_u32(stride);
		};

		frame_metadata_v2_t metadata_v2;
		std::memset(&metadata_v2, 0, sizeof(metadata_v2));
		metadata_v2.header.ensure_magic();
		metadata_v2.header.versions_major            = frame_metadata_v2_header_t::VERSION_MAJOR_V2;
		metadata_v2.header.versions_minor            = VERSION_MINOR;
		metadata_v2.header.frame_id                  = source_metadata.frame_count;
		metadata_v2.header.capture_ts_ns             = source_metadata.timestamp_ns;
		metadata_v2.header.publish_seq               = source_metadata.frame_count;
		metadata_v2.header.plane_count               = 1;
		metadata_v2.header.plane_presence_mask       = 0x01;
		metadata_v2.header.plane_descriptors_offset  = frame_metadata_v2_header_t::PLANE_DESCRIPTORS_OFFSET;
		metadata_v2.header.plane_descriptor_size     = frame_metadata_v2_header_t::PLANE_DESCRIPTOR_SIZE;
		metadata_v2.header.plane_descriptor_capacity = frame_metadata_v2_header_t::PLANE_DESCRIPTOR_CAPACITY;
		metadata_v2.header.payload_size_bytes        = *payload_size_u32;
		metadata_v2.header.depth_unit                = DepthUnit::Unknown;

		const size_t left_expected_stride =
			static_cast<size_t>(source_metadata.info.width) *
			static_cast<size_t>(source_metadata.info.channels) *
			static_cast<size_t>(size_of(source_metadata.info.depth));

		const size_t left_compact_size =
			left_expected_stride * static_cast<size_t>(source_metadata.info.height);

		size_t left_size             = payload_size;
		size_t depth_size            = 0;
		size_t confidence_size       = 0;
		bool depth_plane_active      = false;
		bool confidence_plane_active = false;

		const size_t depth_expected_stride =
			static_cast<size_t>(source_metadata.info.width) * sizeof(float);
		const size_t depth_compact_size =
			depth_expected_stride * static_cast<size_t>(source_metadata.info.height);

		const size_t packed_extra_size =
			payload_size >= left_compact_size ? (payload_size - left_compact_size) : 0;
		const bool has_exact_depth_tail =
			left_compact_size > 0 &&
			depth_compact_size > 0 &&
			payload_size >= left_compact_size &&
			packed_extra_size == depth_compact_size;
		const bool has_exact_depth_and_confidence_tail =
			left_compact_size > 0 &&
			depth_compact_size > 0 &&
			payload_size >= left_compact_size &&
			packed_extra_size == (depth_compact_size * 2);

		// The ZED backend emits compact payloads as:
		// left | depth | optional confidence
		if (has_exact_depth_and_confidence_tail) {
			left_size               = left_compact_size;
			depth_size              = depth_compact_size;
			confidence_size         = depth_compact_size;
			depth_plane_active      = true;
			confidence_plane_active = true;
		} else if (has_exact_depth_tail) {
			left_size          = left_compact_size;
			depth_size         = depth_compact_size;
			depth_plane_active = true;
		}

		auto left_size_u32 = to_u32(left_size);
		if (!left_size_u32) {
			return std::nullopt;
		}

		auto left_stride_u32 = make_stride(left_size, source_metadata.info.height, left_expected_stride);
		if (!left_stride_u32) {
			return std::nullopt;
		}

		auto &left_descriptor        = metadata_v2.plane_descriptors[0];
		left_descriptor.plane_type   = FramePlaneType::LEFT;
		left_descriptor.pixel_format = source_metadata.info.pixel_format;
		left_descriptor.depth        = source_metadata.info.depth;
		left_descriptor.width        = source_metadata.info.width;
		left_descriptor.height       = source_metadata.info.height;
		left_descriptor.stride_bytes = *left_stride_u32;
		left_descriptor.offset_bytes = 0;
		left_descriptor.size_bytes   = *left_size_u32;

		if (depth_plane_active) {
			auto depth_size_u32   = to_u32(depth_size);
			auto depth_offset_u32 = to_u32(left_size);
			if (!depth_size_u32 || !depth_offset_u32) {
				return std::nullopt;
			}

			auto depth_stride_u32 = make_stride(depth_size, source_metadata.info.height, depth_expected_stride);
			if (!depth_stride_u32) {
				return std::nullopt;
			}

			auto &depth_descriptor        = metadata_v2.plane_descriptors[1];
			depth_descriptor.plane_type   = FramePlaneType::DEPTH;
			depth_descriptor.pixel_format = PixelFormat::GRAY;
			depth_descriptor.depth        = Depth::F32;
			depth_descriptor.width        = source_metadata.info.width;
			depth_descriptor.height       = source_metadata.info.height;
			depth_descriptor.stride_bytes = *depth_stride_u32;
			depth_descriptor.offset_bytes = *depth_offset_u32;
			depth_descriptor.size_bytes   = *depth_size_u32;

			metadata_v2.header.plane_count         = 2;
			metadata_v2.header.plane_presence_mask = 0x03;
			metadata_v2.header.depth_unit          = determine_depth_unit();
		}

		if (confidence_plane_active) {
			auto confidence_size_u32 = to_u32(confidence_size);
			auto confidence_offset_u32 = to_u32(left_size + depth_size);
			if (!confidence_size_u32 || !confidence_offset_u32) {
				return std::nullopt;
			}

			auto confidence_stride_u32 =
				make_stride(confidence_size, source_metadata.info.height, depth_expected_stride);
			if (!confidence_stride_u32) {
				return std::nullopt;
			}

			auto &confidence_descriptor        = metadata_v2.plane_descriptors[2];
			confidence_descriptor.plane_type   = FramePlaneType::CONFIDENCE;
			confidence_descriptor.pixel_format = PixelFormat::GRAY;
			confidence_descriptor.depth        = Depth::F32;
			confidence_descriptor.width        = source_metadata.info.width;
			confidence_descriptor.height       = source_metadata.info.height;
			confidence_descriptor.stride_bytes = *confidence_stride_u32;
			confidence_descriptor.offset_bytes = *confidence_offset_u32;
			confidence_descriptor.size_bytes   = *confidence_size_u32;

			metadata_v2.header.plane_count         = 3;
			metadata_v2.header.plane_presence_mask = 0x07;
		}

		return metadata_v2;
	};

	// Create backend based on config
	pro::proxy<app::backends::IBackend> backend;
	switch (config.video.backend) {
	case app::BackendType::Dummy: {
		if (!config.dummy) {
			spdlog::error("Dummy backend selected but [dummy] config section missing");
			return 1;
		}
		backend = pro::make_proxy<app::backends::IBackend, app::backends::DummyBackend>(
			*config.dummy,
			config.video);
		spdlog::info("using Dummy backend");
		break;
	}
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
	case app::BackendType::ZED: {
#ifdef WITH_BACKEND_ZED
		if (!config.zed) {
			spdlog::error("ZED backend selected but [zed] config section missing");
			return 1;
		}
		backend = pro::make_proxy<app::backends::IBackend, app::backends::ZedBackend>(
			*config.zed,
			config.video);
		spdlog::info("using ZED backend");
		break;
#else
		spdlog::error("ZED backend selected but unavailable in this build; reconfigure with -DWITH_BACKEND_ZED=ON");
		return 1;
#endif
	}
	default:
		spdlog::error("selected backend is not available in this build");
		return 1;
	}

	backend->SetOnMetadata([&shm_state, &frame_state, &sync_msg, &config, &build_v2_metadata, &now_ns](const frame_metadata_t &metadata) {
		const auto picture_buffer_size = metadata.info.buffer_size;
		if (picture_buffer_size == 0) {
			spdlog::error("received zero-sized picture buffer in metadata callback");
			return;
		}
		const auto total_buffer_size = SHM_PAYLOAD_OFFSET + picture_buffer_size;

		auto fs = frame_state_t::open(shm_state.fd(), total_buffer_size);
		if (not fs) {
			spdlog::error("open frame state; {}", fs.error());
			return;
		}

		auto initial_metadata         = metadata;
		initial_metadata.frame_count  = 0;
		initial_metadata.timestamp_ns = now_ns();
		auto initial_metadata_v2      = build_v2_metadata(initial_metadata, picture_buffer_size);
		if (!initial_metadata_v2) {
			spdlog::error("build initial ABI v2 metadata failed");
			return;
		}
		fs->write_metadata(*initial_metadata_v2);
		frame_state = std::move(*fs);
		sync_msg.emplace(config.name, 0);
	});

	backend->SetOnFrame([&frame_state, &sync_msg, &sock, &shm_state, &build_v2_metadata, &undistort_pass](std::span<uint8_t> frame_buffer, const frame_metadata_t &metadata) {
		if (not frame_state || not sync_msg) {
			spdlog::error("[BUG] frame callback invoked before metadata callback (should not happen)");
			return;
		}

		std::span<const uint8_t> output_buffer(frame_buffer.data(), frame_buffer.size());
		if (undistort_pass) {
			try {
				output_buffer = undistort_pass->apply(output_buffer, metadata.info);
			} catch (const std::exception &e) {
				spdlog::error("undistort preprocess failed: {}", e.what());
				return;
			}
		}

		// Copy frame data to shared memory
		const auto picture_buffer_size = output_buffer.size();
		if (picture_buffer_size == 0) {
			spdlog::error("received zero-sized frame buffer");
			return;
		}

		if (picture_buffer_size > frame_state->image_buffer().size()) {
			const auto total_buffer_size = SHM_PAYLOAD_OFFSET + picture_buffer_size;
			auto resized_frame_state     = frame_state_t::open(shm_state.fd(), total_buffer_size);
			if (!resized_frame_state) {
				spdlog::error("resize shared memory for frame payload failed; {}", resized_frame_state.error());
				return;
			}
			frame_state = std::move(*resized_frame_state);
		}

		auto metadata_v2 = build_v2_metadata(metadata, picture_buffer_size);
		if (!metadata_v2) {
			spdlog::error("build ABI v2 metadata failed for frame@{}", metadata.frame_count);
			return;
		}

		auto &fs = *frame_state;
		if (metadata_v2->header.payload_size_bytes > fs.image_buffer().size()) {
			spdlog::error("frame payload ({}) exceeds shared memory payload capacity ({})",
						  metadata_v2->header.payload_size_bytes,
						  fs.image_buffer().size());
			return;
		}
		std::copy_n(output_buffer.begin(), metadata_v2->header.payload_size_bytes, fs.image_buffer().begin());
		fs.write_metadata(*metadata_v2);

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

	backend->SetOnBodyTracking([&body_sock, &serialize_body_tracking_frame](const cvmmap::body_tracking_frame_t &frame) {
		if (!body_sock) {
			return;
		}
		try {
			auto bytes = serialize_body_tracking_frame(frame);
			body_sock->send(zmq::buffer(bytes), zmq::send_flags::none);
		} catch (const zmq::error_t &e) {
			spdlog::error("send body tracking message for frame@{}; {}", frame.header.frame_count, e.what());
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

	const auto send_response = [&control_sock, &config](int32_t command_id, int32_t response_code, std::span<const uint8_t> response_message = {}) {
		// Small object optimization: use stack buffer for small messages, heap for large ones
		constexpr size_t SSO_THRESHOLD = 64;
		const size_t total_size        = sizeof(control_message_response_t) + response_message.size();

		alignas(control_message_response_t) uint8_t stack_buffer[sizeof(control_message_response_t) + SSO_THRESHOLD];
		std::unique_ptr<uint8_t[]> heap_buffer;
		uint8_t *buffer_ptr;

		if (response_message.size() <= SSO_THRESHOLD) {
			buffer_ptr = stack_buffer;
		} else {
			heap_buffer = std::make_unique<uint8_t[]>(total_size);
			buffer_ptr  = heap_buffer.get();
		}

		auto *response       = new (buffer_ptr) control_message_response_t{};
		response->command_id = command_id;
		response->set_label(config.name);
		response->response_code           = response_code;
		response->response_message_length = static_cast<uint16_t>(response_message.size());
		if (!response_message.empty()) {
			std::copy(response_message.begin(), response_message.end(), response->_response_message_data);
		}
		try {
			control_sock.send(zmq::buffer(buffer_ptr, total_size), zmq::send_flags::none);
		} catch (const zmq::error_t &e) {
			spdlog::error("send control response: {}", e.what());
		}
	};

	// Main loop: poll for control messages
	while (is_running.load(std::memory_order::relaxed)) {
		zmq::message_t request;
		zmq::recv_result_t result;
		try {
			result = control_sock.recv(request, zmq::recv_flags::none);
		} catch (const zmq::error_t &e) {
			if (e.num() == EINTR) {
				// Interrupted by signal (e.g., SIGINT), check is_running and continue
				continue;
			}
			spdlog::error("recv control message: {}", e.what());
			continue;
		}
		if (!result) {
			// Timeout or no message, continue polling
			continue;
		}

		// Process control message
		if (request.size() < sizeof(control_message_request_t)) {
			spdlog::warn("received control message too small: {} bytes", request.size());
			send_response(CONTROL_MSG_CMD_GENERIC, CONTROL_RESPONSE_INVALID_MSG_SIZE);
			continue;
		}

		const auto *req = static_cast<const control_message_request_t *>(request.data());
		if (req->_magic != CONTROL_MESSAGE_REQUEST_MAGIC) {
			spdlog::warn("received control message with invalid magic: 0x{:02x}", req->_magic);
			send_response(req->command_id, CONTROL_RESPONSE_INVALID_MAGIC);
			continue;
		}

		spdlog::debug("received control message: command_id=0x{:04x}", req->command_id);

		// Migration policy: shared-memory metadata may move to major v2 while
		// sync/control wire messages stay at major v1; reject other control majors deterministically.
		if (req->versions_major != VERSION_MAJOR) {
			spdlog::warn("received control message with incompatible version: {}.{} (expected control {}.x)",
						 req->versions_major, req->versions_minor, VERSION_MAJOR);
			send_response(req->command_id, CONTROL_RESPONSE_INVALID_VERSION);
			continue;
		}

		if (req->label() != config.name) {
			spdlog::warn("received control message for different instance: `{}` != `{}`", req->label(), config.name);
			send_response(req->command_id, CONTROL_RESPONSE_INVALID_LABEL);
			continue;
		}

		switch (req->command_id) {
		case CONTROL_MSG_CMD_RESET_FRAME_COUNT: {
			spdlog::info("control: RESET_FRAME_COUNT requested");
			auto err = backend->ResetFrameCount();
			if (err != backends::ERR_OK) {
				spdlog::error("resetting frame count: {}", err);
				send_response(req->command_id, CONTROL_RESPONSE_ERROR);
			} else {
				send_status(MODULE_STATUS_STREAM_RESET);
				send_response(req->command_id, CONTROL_RESPONSE_OK);
			}
			break;
		}
		default:
			spdlog::warn("unknown control command: 0x{:04x}", req->command_id);
			send_response(req->command_id, CONTROL_RESPONSE_UNKNOWN_CMD);
			break;
		}
	}

	backend->Shutdown();
	send_status(MODULE_STATUS_OFFLINE);

	spdlog::info("normally exit");
	return 0;
}
