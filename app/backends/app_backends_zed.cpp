#include <errno.h>
#include <memory>
#include <utility>

#if defined(WITH_BACKEND_ZED) && __has_include(<sl/Camera.hpp>)
#include <sl/Camera.hpp>
#define APP_HAS_ZED_SDK 1
#else
#define APP_HAS_ZED_SDK 0
#endif

#if APP_HAS_ZED_SDK
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#endif

#include <spdlog/spdlog.h>

#include "app_backends_facade.hpp"
#include "app_backends_zed.hpp"
#if APP_HAS_ZED_SDK
#include "app_config.hpp"
#include "app_enum_models.hpp"
#endif

namespace app::backends {

#if APP_HAS_ZED_SDK

namespace {

std::string normalize_ascii_lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

bool is_network_stream_mode(const std::string_view mode) {
	const auto normalized = normalize_ascii_lower(std::string(mode));
	return normalized == "network" || normalized == "ethernet" || normalized == "stream";
}

sl::RESOLUTION parse_resolution(std::string value) {
	value = normalize_ascii_lower(std::move(value));
	if (value == "hd2k") {
		return sl::RESOLUTION::HD2K;
	}
	if (value == "hd1200") {
		return sl::RESOLUTION::HD1200;
	}
	if (value == "hd1080") {
		return sl::RESOLUTION::HD1080;
	}
	if (value == "hd720") {
		return sl::RESOLUTION::HD720;
	}
	if (value == "svga") {
		return sl::RESOLUTION::SVGA;
	}
	if (value == "vga") {
		return sl::RESOLUTION::VGA;
	}
	if (value == "auto") {
		return sl::RESOLUTION::AUTO;
	}

	spdlog::warn("unknown ZED resolution '{}', fallback to HD720", value);
	return sl::RESOLUTION::HD720;
}

sl::DEPTH_MODE parse_depth_mode(std::string value) {
	value = normalize_ascii_lower(std::move(value));
	if (value == "none") {
		return sl::DEPTH_MODE::NONE;
	}
	if (value == "performance") {
		return sl::DEPTH_MODE::PERFORMANCE;
	}
	if (value == "quality") {
		return sl::DEPTH_MODE::QUALITY;
	}
	if (value == "ultra") {
		return sl::DEPTH_MODE::ULTRA;
	}

	spdlog::warn("unknown ZED depth mode '{}', fallback to PERFORMANCE", value);
	return sl::DEPTH_MODE::PERFORMANCE;
}

sl::VIEW parse_left_view(std::string value) {
	value = normalize_ascii_lower(std::move(value));
	if (value == "gray8" || value == "mono8") {
		return sl::VIEW::LEFT_GRAY;
	}
	return sl::VIEW::LEFT;
}

PixelFormat guess_pixel_format_for_zed(const sl::MAT_TYPE mat_type, const std::string &left_pixel_format) {
	const auto format_hint = normalize_ascii_lower(left_pixel_format);

	switch (mat_type) {
	case sl::MAT_TYPE::U8_C1:
		return PixelFormat::GRAY;
	case sl::MAT_TYPE::U8_C2:
		return PixelFormat::YUYV;
	case sl::MAT_TYPE::U8_C3:
		if (format_hint == "rgb8") {
			return PixelFormat::RGB;
		}
		return PixelFormat::BGR;
	case sl::MAT_TYPE::U8_C4:
		if (format_hint == "rgba8") {
			return PixelFormat::RGBA;
		}
		return PixelFormat::BGRA;
	default:
		return PixelFormat::BGR;
	}
}

uint8_t channels_from_mat_type(const sl::MAT_TYPE mat_type) {
	switch (mat_type) {
	case sl::MAT_TYPE::U8_C1:
		return 1;
	case sl::MAT_TYPE::U8_C2:
		return 2;
	case sl::MAT_TYPE::U8_C3:
		return 3;
	case sl::MAT_TYPE::U8_C4:
		return 4;
	default:
		return 0;
	}
}

uint64_t now_ns() {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

}

struct ZedBackendOptions {
	app::ZedConfig zed_config;
	app::VideoConfig video_config;
};

struct ZedBackendImpl {
	ZedBackendOptions options;
	sl::Camera camera;
	sl::InitParameters init_parameters{};
	sl::RuntimeParameters runtime_parameters{};
	sl::VIEW left_view{sl::VIEW::LEFT};
	sl::Mat left_frame;
	std::jthread worker_thread;
	std::atomic<bool> initialized{false};

	on_metadata_fn_t _on_metadata{nullptr};
	on_frame_fn_t _on_frame{nullptr};
	on_error_fn_t _on_error{nullptr};

	frame_metadata_t metadata{};
	int consecutive_failures{0};

	ZedBackendImpl() = default;
	explicit ZedBackendImpl(ZedBackendOptions opts) : options(std::move(opts)) {}

	~ZedBackendImpl() {
		Shutdown();
	}

	void on_metadata(const frame_metadata_t &m) {
		if (_on_metadata) {
			_on_metadata(m);
		}
	}

	void on_frame(std::span<uint8_t> frame_buffer, const frame_metadata_t &m) {
		if (_on_frame) {
			_on_frame(frame_buffer, m);
		}
	}

	void on_error(error_t error_code, std::string_view message) {
		if (_on_error) {
			_on_error(error_code, message);
		}
	}

	bool open_camera() {
		init_parameters                  = sl::InitParameters{};
		init_parameters.camera_resolution = parse_resolution(options.zed_config.resolution);
		init_parameters.camera_fps        = options.zed_config.fps;
		init_parameters.depth_mode        = parse_depth_mode(options.zed_config.depth_mode);

		const auto stream_mode = normalize_ascii_lower(options.zed_config.stream_mode);
		if (is_network_stream_mode(stream_mode)) {
			if (!options.zed_config.ip_address || options.zed_config.ip_address->empty()) {
				spdlog::error("ZED stream_mode='{}' requires ip_address", options.zed_config.stream_mode);
				return false;
			}

			if (options.zed_config.port) {
				spdlog::info("opening ZED network stream {}:{}", *options.zed_config.ip_address, *options.zed_config.port);
				init_parameters.input.setFromStream(sl::String(options.zed_config.ip_address->c_str()), static_cast<unsigned short>(*options.zed_config.port));
			} else {
				spdlog::info("opening ZED network stream {}", *options.zed_config.ip_address);
				init_parameters.input.setFromStream(sl::String(options.zed_config.ip_address->c_str()));
			}
		} else {
			if (options.zed_config.serial) {
				spdlog::info("opening ZED local camera by serial={}", *options.zed_config.serial);
				init_parameters.input.setFromSerialNumber(*options.zed_config.serial);
			} else if (options.zed_config.index) {
				spdlog::info("opening ZED local camera by index={}", *options.zed_config.index);
				init_parameters.input.setFromCameraID(*options.zed_config.index);
			} else {
				spdlog::info("opening ZED local camera by default selection");
			}
		}

		left_view = parse_left_view(options.zed_config.left_pixel_format);

		auto open_result     = sl::ERROR_CODE::FAILURE;
		const auto timeout_ms = std::max(1, options.zed_config.open_timeout_ms);
		auto started         = std::chrono::steady_clock::now();

		while (true) {
			open_result = camera.open(init_parameters);
			if (open_result == sl::ERROR_CODE::SUCCESS) {
				spdlog::info("ZED camera opened");
				return true;
			}

			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started)
							 .count();
			if (elapsed >= timeout_ms) {
				break;
			}

			if (camera.isOpened()) {
				camera.close();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
		}

		spdlog::error("failed to open ZED camera within {}ms: code={}", timeout_ms, static_cast<int>(open_result));
		return false;
	}

	void warmup_camera() {
		const auto warmup_frames = std::max(0, options.zed_config.warmup_frames);
		for (int i = 0; i < warmup_frames; i++) {
			camera.grab(runtime_parameters);
		}
	}

	bool capture_frame(sl::Mat &dst) {
		const auto grab_result = camera.grab(runtime_parameters);
		if (grab_result != sl::ERROR_CODE::SUCCESS) {
			spdlog::debug("ZED grab failed: code={}", static_cast<int>(grab_result));
			return false;
		}

		const auto retrieve_result = camera.retrieveImage(dst, left_view, sl::MEM::CPU);
		if (retrieve_result != sl::ERROR_CODE::SUCCESS) {
			spdlog::debug("ZED retrieveImage failed: code={}", static_cast<int>(retrieve_result));
			return false;
		}

		const auto frame_size = frame_size_bytes(dst);
		auto *frame_ptr       = dst.getPtr<sl::uchar1>(sl::MEM::CPU);
		if (!frame_ptr || frame_size == 0) {
			spdlog::warn("ZED frame pointer invalid or frame size is zero");
			return false;
		}

		return true;
	}

	std::optional<frame_info_t> make_frame_info(const sl::Mat &frame) {
		const auto width  = frame.getWidth();
		const auto height = frame.getHeight();
		if (width <= 0 || height <= 0 ||
			width > std::numeric_limits<uint16_t>::max() ||
			height > std::numeric_limits<uint16_t>::max()) {
			spdlog::error("invalid ZED frame dimensions: {}x{}", width, height);
			return std::nullopt;
		}

		const auto mat_type = frame.getDataType();
		const auto channels = channels_from_mat_type(mat_type);
		if (channels == 0) {
			spdlog::error("unsupported ZED MAT_TYPE: {}", static_cast<int>(mat_type));
			return std::nullopt;
		}

		const auto buffer_size = frame_size_bytes(frame);
		if (buffer_size == 0 || buffer_size > std::numeric_limits<uint32_t>::max()) {
			spdlog::error("invalid ZED frame buffer size: {}", buffer_size);
			return std::nullopt;
		}

		return frame_info_t{
			.width        = static_cast<uint16_t>(width),
			.height       = static_cast<uint16_t>(height),
			.channels     = channels,
			.depth        = Depth::U8,
			.pixel_format = guess_pixel_format_for_zed(mat_type, options.zed_config.left_pixel_format),
			.buffer_size  = static_cast<uint32_t>(buffer_size),
		};
	}

	size_t frame_size_bytes(const sl::Mat &frame) const {
		const auto step_bytes = frame.getStepBytes(sl::MEM::CPU);
		const auto height     = frame.getHeight();
		if (step_bytes <= 0 || height <= 0) {
			return 0;
		}
		return static_cast<size_t>(step_bytes) * static_cast<size_t>(height);
	}

	bool reconnect() {
		if (camera.isOpened()) {
			camera.close();
		}

		const auto sleep_ms = std::max(1, options.zed_config.reconnect_interval_ms);
		std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));

		if (!open_camera()) {
			return false;
		}

		warmup_camera();
		consecutive_failures = 0;
		return true;
	}

	void Init() {
		if (initialized.load(std::memory_order_relaxed)) {
			return;
		}

		if (!open_camera()) {
			on_error(-ENODEV, "Failed to open ZED camera");
			return;
		}

		warmup_camera();

		if (!capture_frame(left_frame)) {
			spdlog::error("failed to capture first frame from ZED");
			on_error(-EIO, "Failed to capture first frame");
			camera.close();
			return;
		}

		auto info = make_frame_info(left_frame);
		if (!info) {
			on_error(-EINVAL, "Unsupported ZED frame format");
			camera.close();
			return;
		}

		metadata.frame_count = 0;
		metadata.info        = *info;
		metadata.timestamp_ns = now_ns();

		spdlog::info("initial ZED frame info: {}x{}x{}; depth={}; bufferSize={}; pixelFormat={}",
					 metadata.info.width,
					 metadata.info.height,
					 metadata.info.channels,
					 app::to_str(metadata.info.depth),
					 metadata.info.buffer_size,
					 app::to_str(metadata.info.pixel_format));

		on_metadata(metadata);

		auto *frame_ptr = left_frame.getPtr<sl::uchar1>(sl::MEM::CPU);
		auto frame_size = frame_size_bytes(left_frame);
		on_frame(std::span<uint8_t>(frame_ptr, frame_size), metadata);

		initialized.store(true, std::memory_order_relaxed);
		worker_thread = std::jthread([this](std::stop_token stop_token) {
			worker_loop(stop_token);
		});
	}

	void worker_loop(std::stop_token stop_token) {
		const auto max_failures = std::max(1, options.zed_config.max_consecutive_failures);

		while (!stop_token.stop_requested()) {
			if (capture_frame(left_frame)) {
				consecutive_failures = 0;
				metadata.frame_count += 1;
				metadata.timestamp_ns = now_ns();

				auto *frame_ptr = left_frame.getPtr<sl::uchar1>(sl::MEM::CPU);
				auto frame_size = frame_size_bytes(left_frame);
				on_frame(std::span<uint8_t>(frame_ptr, frame_size), metadata);
				spdlog::debug("frame@{}", metadata.frame_count);
				continue;
			}

			consecutive_failures++;
			if (consecutive_failures < max_failures) {
				continue;
			}

			spdlog::error("ZED capture failed {} consecutive times", consecutive_failures);
			if (!options.zed_config.reconnect) {
				on_error(-EIO, "ZED capture failure");
				break;
			}

			spdlog::warn("attempting ZED reconnect");
			if (!reconnect()) {
				on_error(-EIO, "ZED reconnect failed");
				continue;
			}
		}
	}

	void Shutdown() {
		if (worker_thread.joinable()) {
			worker_thread.request_stop();
			worker_thread.join();
		}

		if (camera.isOpened()) {
			camera.close();
		}

		initialized.store(false, std::memory_order_relaxed);
	}

	void SetOnMetadata(on_metadata_fn_t on_metadata_) {
		_on_metadata = std::move(on_metadata_);
	}

	void SetOnFrame(on_frame_fn_t on_frame_) {
		_on_frame = std::move(on_frame_);
	}

	void SetOnError(on_error_fn_t on_error_) {
		_on_error = std::move(on_error_);
	}

	error_t SeekFrame(size_t) {
		return -EOPNOTSUPP;
	}

	error_t ResetFrameCount() {
		metadata.frame_count = 0;
		return 0;
	}
};

ZedBackend::ZedBackend(const app::ZedConfig &zed_config, const app::VideoConfig &video_config)
	: impl(std::make_unique<ZedBackendImpl>(ZedBackendOptions{
		  .zed_config   = zed_config,
		  .video_config = video_config,
	  })) {}

ZedBackend::~ZedBackend() = default;

void ZedBackend::Init() {
	impl->Init();
}

void ZedBackend::Shutdown() {
	impl->Shutdown();
}

void ZedBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void ZedBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

void ZedBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

error_t ZedBackend::SeekFrame(size_t frame_index) {
	return impl->SeekFrame(frame_index);
}

error_t ZedBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

#else

struct ZedBackendImpl {
	on_metadata_fn_t _on_metadata{nullptr};
	on_frame_fn_t _on_frame{nullptr};
	on_error_fn_t _on_error{nullptr};

	void Init() {
		spdlog::error("ZED SDK headers not found at compile time");
		if (_on_error) {
			_on_error(-ENODEV, "ZED SDK not available in this build environment");
		}
	}

	void Shutdown() {}

	void SetOnMetadata(on_metadata_fn_t on_metadata_) {
		_on_metadata = std::move(on_metadata_);
	}

	void SetOnFrame(on_frame_fn_t on_frame_) {
		_on_frame = std::move(on_frame_);
	}

	void SetOnError(on_error_fn_t on_error_) {
		_on_error = std::move(on_error_);
	}

	error_t SeekFrame(size_t) {
		return -EOPNOTSUPP;
	}

	error_t ResetFrameCount() {
		return 0;
	}
};

ZedBackend::ZedBackend(const app::ZedConfig &, const app::VideoConfig &)
	: impl(std::make_unique<ZedBackendImpl>()) {}

ZedBackend::~ZedBackend() = default;

void ZedBackend::Init() {
	impl->Init();
}

void ZedBackend::Shutdown() {
	impl->Shutdown();
}

void ZedBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void ZedBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

void ZedBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

error_t ZedBackend::SeekFrame(size_t frame_index) {
	return impl->SeekFrame(frame_index);
}

error_t ZedBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

#endif

}
