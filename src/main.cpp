#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cvmmap/compat/format.hpp>
#include <csignal>
#include <limits>
#include <optional>
#include <functional>
#include <string_view>
#include <string>
#include <cvmmap/compat/expected.hpp>
#include <mutex>
#include <thread>
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
#include "app_utils.hpp"
#include "backends/app_backends_handle.hpp"
#include "app_preprocess_undistort.hpp"
#include <cvmmap/nats_service.hpp>

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

	const auto resolved_target = cvmmap::resolve_cvmmap_target_or_throw(
		cvmmap::format(
			"cvmmap://{}@{}?namespace={}",
			config.name,
			config.ipc.prefix,
			config.ipc.name_space));

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

	const bool nats_enabled = config.nats.enabled;
	std::unique_ptr<cvmmap::NatsControlService> nats_service;
	if (nats_enabled) {
		nats_service = std::make_unique<cvmmap::NatsControlService>(
			config.name,
			resolved_target.nats_target_key,
			config.nats.url);
	} else {
		spdlog::warn("NATS disabled; control/status and body-tracking transport are unavailable");
	}

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

		static cvmmap::expected<shm_state_t, int> open(const std::string &name) {
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
						return cvmmap::unexpected(errno);
					} else {
						spdlog::warn("unlinked shared memory `{}`", name);
						return shm_state_t::open(name);
					}
				}
				return cvmmap::unexpected(errno);
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

		static cvmmap::expected<frame_state_t, int> open(int shm_fd, size_t size) {
			// https://www.deepanseeralan.com/tech/playing-with-shared-memory/
			// ftruncate first, then mmap
			if (ftruncate(shm_fd, size) == -1) {
				spdlog::error("truncate shared memory; fd={}, errno={} ({})", shm_fd, errno, strerror(errno));
				return cvmmap::unexpected(errno);
			}
			auto ptr = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
			if (ptr == MAP_FAILED) {
				// https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/mmap.2.html
				spdlog::error("mmap shared memory; fd={}, errno={} ({})", shm_fd, errno, strerror(errno));
				return cvmmap::unexpected(errno);
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
	app::backends::BackendHandle backend;
	switch (config.video.backend) {
	case app::BackendType::Dummy: {
		if (!config.dummy) {
			spdlog::error("Dummy backend selected but [dummy] config section missing");
			return 1;
		}
		backend.emplace<app::backends::DummyBackend>(
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
		backend.emplace<app::backends::OpenCVBackend>(
			config.opencv->parameter,
			config.video,
			config.opencv->api_preference);
		spdlog::info("using OpenCV backend");
		break;
	}
#else
	case app::BackendType::OpenCV: {
		spdlog::error("OpenCV backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_OPENCV=ON");
		return 1;
	}
#endif
#ifdef WITH_BACKEND_GSTREAMER
	case app::BackendType::GStreamer: {
		if (!config.gstreamer) {
			spdlog::error("GStreamer backend selected but [gstreamer] config section missing");
			return 1;
		}
		backend.emplace<app::backends::GStreamerBackend>(
			config.gstreamer->pipeline,
			config.video);
		spdlog::info("using GStreamer backend");
		break;
	}
#else
	case app::BackendType::GStreamer: {
		spdlog::error("GStreamer backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_GSTREAMER=ON");
		return 1;
	}
#endif
#ifdef WITH_BACKEND_MCAP
	case app::BackendType::MCAP: {
		if (!config.mcap) {
			spdlog::error("MCAP backend selected but [mcap] config section missing");
			return 1;
		}
		backend.emplace<app::backends::McapBackend>(
			*config.mcap,
			config.video);
		spdlog::info("using MCAP backend");
		break;
	}
#else
	case app::BackendType::MCAP: {
		spdlog::error("MCAP backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_MCAP=ON");
		return 1;
	}
#endif
	case app::BackendType::ZED: {
#ifdef WITH_BACKEND_ZED
		if (!config.zed) {
			spdlog::error("ZED backend selected but [zed] config section missing");
			return 1;
		}
		backend.emplace<app::backends::ZedBackend>(
			*config.zed,
			config.video);
		spdlog::info("using ZED backend");
		break;
#else
		spdlog::error("ZED backend selected but unavailable in this build; reconfigure with -DBUILD_BACKEND_ZED=ON");
		return 1;
#endif
	}
	default:
		spdlog::error("selected backend is not available in this build");
		return 1;
	}

	backend.SetOnMetadata([&shm_state, &frame_state, &sync_msg, &config, &build_v2_metadata, &now_ns](const frame_metadata_t &metadata) {
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

	backend.SetOnFrame([&frame_state, &sync_msg, &sock, &shm_state, &build_v2_metadata, &undistort_pass](std::span<uint8_t> frame_buffer, const frame_metadata_t &metadata) {
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

	backend.SetOnBodyTracking([&serialize_body_tracking_frame, &nats_service, nats_enabled](const cvmmap::body_tracking_frame_t &frame) {
			if (!nats_enabled || !nats_service) {
				return;
			}
			auto bytes = serialize_body_tracking_frame(frame);
			nats_service->PublishBodyTracking(
				std::span<const uint8_t>(bytes.data(), bytes.size()));
		});

	const auto send_status = [&nats_service, nats_enabled](int32_t status) {
		if (!nats_enabled || !nats_service) {
			return;
		}
		nats_service->PublishModuleStatus(status);
	};

	backend.SetOnError([&backend, send_status](int error_code, std::string_view message) {
		if (error_code == backends::ERR_EOS) {
			spdlog::info("backend EOF: {}", message);
			const auto source_info = backend.GetSourceInfo();
			if ((source_info.flags & cvmmap::SOURCE_INFO_FLAG_LOOP_EMITS_RESET) != 0) {
				spdlog::info("looping finite stream (encore)");
				auto err = backend.ResetFrameCount();
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

	// Mutex to protect backend calls from concurrent NATS and ZMQ threads
	std::mutex backend_control_mutex;

	const auto seek_timestamp = [&backend](const uint64_t timestamp_ns)
		-> cvmmap::expected<backends::seek_result_t, backends::error_t> {
		return backend.SeekTimestampNs(timestamp_ns);
	};

	const auto map_recording_error = [](const int error_code,
									 std::string message = {}) {
		auto control_code = cvmmap::CONTROL_RESPONSE_ERROR;
		switch (error_code) {
		case 0:
			control_code = cvmmap::CONTROL_RESPONSE_OK;
			break;
		case -EOPNOTSUPP:
			control_code = cvmmap::CONTROL_RESPONSE_UNSUPPORTED;
			break;
		case -EINVAL:
			control_code = cvmmap::CONTROL_RESPONSE_INVALID_PAYLOAD;
			break;
		case -ERANGE:
			control_code = cvmmap::CONTROL_RESPONSE_OUT_OF_RANGE;
			break;
		default:
			control_code = cvmmap::CONTROL_RESPONSE_ERROR;
			break;
		}
		return cvmmap::ControlError{
			.code = control_code,
			.message = std::move(message),
		};
	};

	const auto to_public_recording_status = [](const backends::recording_status_t &status) {
		return cvmmap::RecordingStatus{
			.format = status.format,
			.can_record = status.can_record,
			.is_recording = status.is_recording,
			.is_paused = status.is_paused,
			.last_frame_ok = status.last_frame_ok,
			.frames_ingested = status.frames_ingested,
			.frames_encoded = status.frames_encoded,
			.active_path = status.active_path,
		};
	};

	struct RecorderProvider {
		cvmmap::RecordingFormat format{cvmmap::RecordingFormat::Unknown};
		std::function<bool()> is_available;
		std::function<cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError>(const cvmmap::RecordingRequest &)> start;
		std::function<cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError>()> stop;
		std::function<cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError>()> status;
	};

	std::vector<RecorderProvider> recorder_providers{};

#ifdef WITH_BACKEND_ZED
	if (auto *zed_backend = backend.get_if<app::backends::ZedBackend>()) {
		recorder_providers.push_back(RecorderProvider{
			.format = cvmmap::RecordingFormat::Svo,
			.is_available = [zed_backend]() {
				auto status = zed_backend->GetRecordingStatus();
				return status && status->can_record;
			},
			.start = [zed_backend, &map_recording_error, &to_public_recording_status](
						const cvmmap::RecordingRequest &request)
				-> cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError> {
				backends::svo_recording_request_t backend_request{
					.output_path = request.output_path,
				};
				if (request.svo_options) {
					backend_request.options.compression_mode = request.svo_options->compression_mode;
					backend_request.options.bitrate = request.svo_options->bitrate;
					backend_request.options.target_framerate = request.svo_options->target_framerate;
					backend_request.options.transcode_streaming_input =
						request.svo_options->transcode_streaming_input;
				}
				auto result = zed_backend->StartRecording(backend_request);
				if (!result) {
					return cvmmap::unexpected(
						map_recording_error(result.error(), zed_backend->GetLastRecordingError()));
				}
				return to_public_recording_status(*result);
			},
			.stop = [zed_backend, &map_recording_error, &to_public_recording_status]()
				-> cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError> {
				auto result = zed_backend->StopRecording();
				if (!result) {
					return cvmmap::unexpected(
						map_recording_error(result.error(), zed_backend->GetLastRecordingError()));
				}
				return to_public_recording_status(*result);
			},
			.status = [zed_backend, &map_recording_error, &to_public_recording_status]()
				-> cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError> {
				auto result = zed_backend->GetRecordingStatus();
				if (!result) {
					return cvmmap::unexpected(
						map_recording_error(result.error(), zed_backend->GetLastRecordingError()));
				}
				return to_public_recording_status(*result);
			},
		});
	}
#endif

	const auto find_recorder_provider = [&recorder_providers](const cvmmap::RecordingFormat format)
		-> RecorderProvider * {
		for (auto &provider : recorder_providers) {
			if (provider.format == format) {
				return &provider;
			}
		}
		return nullptr;
	};

	backend.Init();

	// Wire up NATS handlers and start service only when transport is enabled.
	if (nats_enabled) {
		cvmmap::NatsControlHandlers nats_handlers;
		nats_handlers.on_reset_frame_count = [&backend, &backend_control_mutex]() -> int {
			std::lock_guard lock(backend_control_mutex);
			return backend.ResetFrameCount();
		};
		nats_handlers.on_get_source_info = [&backend, &backend_control_mutex, &recorder_providers]() {
			std::lock_guard lock(backend_control_mutex);
			auto info = backend.GetSourceInfo();
			info.flags &= ~cvmmap::SOURCE_INFO_FLAG_CAN_RECORD;
			for (const auto &provider : recorder_providers) {
				if (provider.is_available && provider.is_available()) {
					info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_RECORD;
					break;
				}
			}
			return info;
		};
		nats_handlers.on_seek_timestamp = [&backend_control_mutex, &seek_timestamp](uint64_t ts) {
			std::lock_guard lock(backend_control_mutex);
			return seek_timestamp(ts);
		};
		nats_handlers.on_recording_available =
			[&backend_control_mutex, &find_recorder_provider](const cvmmap::RecordingFormat format) {
			std::lock_guard lock(backend_control_mutex);
			auto *provider = find_recorder_provider(format);
			return provider && provider->is_available && provider->is_available();
		};
		nats_handlers.on_start_recording =
			[&backend_control_mutex, &find_recorder_provider](const cvmmap::RecordingRequest &request)
				-> cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError> {
			std::lock_guard lock(backend_control_mutex);
			auto *provider = find_recorder_provider(request.format);
			if (!provider || !provider->start) {
				return cvmmap::unexpected(cvmmap::ControlError{
					.code = cvmmap::CONTROL_RESPONSE_UNSUPPORTED,
					.message = "recording format is not supported by the active producer",
				});
			}
			return provider->start(request);
		};
		nats_handlers.on_stop_recording =
			[&backend_control_mutex, &find_recorder_provider](const cvmmap::RecordingFormat format)
				-> cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError> {
			std::lock_guard lock(backend_control_mutex);
			auto *provider = find_recorder_provider(format);
			if (!provider || !provider->stop) {
				return cvmmap::unexpected(cvmmap::ControlError{
					.code = cvmmap::CONTROL_RESPONSE_UNSUPPORTED,
					.message = "recording format is not supported by the active producer",
				});
			}
			return provider->stop();
		};
		nats_handlers.on_get_recording_status =
			[&backend_control_mutex, &find_recorder_provider](const cvmmap::RecordingFormat format)
				-> cvmmap::expected<cvmmap::RecordingStatus, cvmmap::ControlError> {
			std::lock_guard lock(backend_control_mutex);
			auto *provider = find_recorder_provider(format);
			if (!provider || !provider->status) {
				return cvmmap::unexpected(cvmmap::ControlError{
					.code = cvmmap::CONTROL_RESPONSE_UNSUPPORTED,
					.message = "recording format is not supported by the active producer",
				});
			}
			return provider->status();
		};
		nats_service->SetHandlers(std::move(nats_handlers));
		if (!nats_service->Start()) {
			spdlog::error("failed to start NATS control service on '{}'", config.nats.url);
			backend.Shutdown();
			return 1;
		}
	}

	send_status(MODULE_STATUS_ONLINE);
	while (is_running.load(std::memory_order::relaxed)) {
		std::this_thread::sleep_for(std::chrono::milliseconds{100});
	}

	backend.Shutdown();
	send_status(MODULE_STATUS_OFFLINE);
	if (nats_service) {
		nats_service->Stop();
	}

	spdlog::info("normally exit");
	return 0;
}
