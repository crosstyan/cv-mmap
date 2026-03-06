#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include <spdlog/spdlog.h>

#include "app_preprocess_undistort.hpp"

namespace {

app::frame_info_t make_frame_info(const uint16_t width, const uint16_t height, const uint8_t channels) {
	return app::frame_info_t{
		.width        = width,
		.height       = height,
		.channels     = channels,
		.depth        = app::Depth::U8,
		.pixel_format = channels == 1 ? app::PixelFormat::GRAY : (channels == 4 ? app::PixelFormat::BGRA : app::PixelFormat::BGR),
		._reserved_0  = {0},
		.buffer_size  = static_cast<uint32_t>(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels))};
}

app::UndistortConfig make_config() {
	app::UndistortConfig cfg;
	cfg.enabled                       = true;
	cfg.camera_matrix                 = {850.0, 0.0, 640.0,
										 0.0, 850.0, 360.0,
										 0.0, 0.0, 1.0};
	cfg.dist_coeffs                   = {-0.18, 0.045, 0.0007, -0.0004, 0.0};
	cfg.use_optimal_new_camera_matrix = true;
	cfg.alpha                         = 0.0;
	return cfg;
}

std::vector<uint8_t> make_frame_buffer(const app::frame_info_t &info) {
	std::vector<uint8_t> buffer(info.buffer_size);
	for (size_t i = 0; i < buffer.size(); ++i) {
		buffer[i] = static_cast<uint8_t>((i * 17U + 29U) % 255U);
	}
	return buffer;
}

double benchmark_current_build(const app::frame_info_t &info, const std::vector<uint8_t> &input, const int iterations) {
	app::preprocess::UndistortPass pass(make_config());
	std::span<const uint8_t> output;
	const auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < iterations; ++i) {
		output = pass.apply(std::span<const uint8_t>(input.data(), input.size()), info);
	}
	const auto end = std::chrono::steady_clock::now();
	if (output.size() != input.size()) {
		throw std::runtime_error("unexpected output size from undistort benchmark");
	}
	return std::chrono::duration<double, std::milli>(end - start).count() / static_cast<double>(iterations);
}

}

int main(int argc, char **argv) {
	const int width      = argc > 1 ? std::atoi(argv[1]) : 1280;
	const int height     = argc > 2 ? std::atoi(argv[2]) : 720;
	const int iterations = argc > 3 ? std::atoi(argv[3]) : 200;

	if (width <= 0 || height <= 0 || iterations <= 0) {
		std::cerr << "usage: app_preprocess_benchmark [width] [height] [iterations]\n";
		return 2;
	}

	spdlog::set_level(spdlog::level::warn);
	const auto info  = make_frame_info(static_cast<uint16_t>(width), static_cast<uint16_t>(height), 3);
	const auto input = make_frame_buffer(info);

	const auto avg_ms = benchmark_current_build(info, input, iterations);

#ifdef WITH_PREPROCESS_UNDISTORT_OPENCV
	std::cout << "opencv avg_ms=" << avg_ms << "\n";
#else
	std::cout << "internal avg_ms=" << avg_ms << "\n";
#endif

	return 0;
}
