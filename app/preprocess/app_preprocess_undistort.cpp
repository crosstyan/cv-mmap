#include "app_preprocess_undistort.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace app::preprocess {
namespace {
	const char *kAttributionNote =
		"Undistortion equations adapted from OpenCV calib3d implementation (Brown-Conrady mapping logic), "
		"implemented here without linking OpenCV for preprocess path. "
		"Reference: opencv/modules/calib3d/src/undistort.dispatch.cpp";

	struct DistortionParams {
		double k1{0.0};
		double k2{0.0};
		double p1{0.0};
		double p2{0.0};
		double k3{0.0};
		double k4{0.0};
		double k5{0.0};
		double k6{0.0};
		double s1{0.0};
		double s2{0.0};
		double s3{0.0};
		double s4{0.0};
	};

	inline uint8_t sample_u8_bilinear(const uint8_t *src, int width, int height, int channels, float x, float y, int c) {
		if (x < 0.0F || y < 0.0F || x > static_cast<float>(width - 1) || y > static_cast<float>(height - 1)) {
			return 0;
		}
		const int x0 = static_cast<int>(std::floor(x));
		const int y0 = static_cast<int>(std::floor(y));
		const int x1 = std::min(x0 + 1, width - 1);
		const int y1 = std::min(y0 + 1, height - 1);

		const float wx = x - static_cast<float>(x0);
		const float wy = y - static_cast<float>(y0);

		const size_t idx00 = static_cast<size_t>((y0 * width + x0) * channels + c);
		const size_t idx10 = static_cast<size_t>((y0 * width + x1) * channels + c);
		const size_t idx01 = static_cast<size_t>((y1 * width + x0) * channels + c);
		const size_t idx11 = static_cast<size_t>((y1 * width + x1) * channels + c);

		const float v00 = static_cast<float>(src[idx00]);
		const float v10 = static_cast<float>(src[idx10]);
		const float v01 = static_cast<float>(src[idx01]);
		const float v11 = static_cast<float>(src[idx11]);

		const float v0 = v00 * (1.0F - wx) + v10 * wx;
		const float v1 = v01 * (1.0F - wx) + v11 * wx;
		const float v  = v0 * (1.0F - wy) + v1 * wy;

		return static_cast<uint8_t>(std::clamp(v, 0.0F, 255.0F));
	}
}

UndistortPass::UndistortPass(app::UndistortConfig config) : config_(std::move(config)) {}

bool UndistortPass::enabled() const {
	return config_.enabled && active_;
}

bool UndistortPass::supports(const app::frame_info_t &info) const {
	if (info.depth != app::Depth::U8) {
		return false;
	}
	if (info.channels != 1 && info.channels != 3 && info.channels != 4) {
		return false;
	}
	if (info.pixel_format == app::PixelFormat::YUV || info.pixel_format == app::PixelFormat::YUYV) {
		return false;
	}
	return true;
}

size_t UndistortPass::expected_buffer_size(const app::frame_info_t &info) const {
	return static_cast<size_t>(info.width) * static_cast<size_t>(info.height) * static_cast<size_t>(info.channels);
}

void UndistortPass::disable(std::string reason) {
	if (!active_) {
		return;
	}
	active_          = false;
	disabled_reason_ = std::move(reason);
	spdlog::warn("undistort disabled: {}", disabled_reason_);
}

void UndistortPass::build_map(const app::frame_info_t &info) {
	const int width  = static_cast<int>(info.width);
	const int height = static_cast<int>(info.height);
	if (width <= 0 || height <= 0) {
		throw std::invalid_argument("invalid frame dimensions");
	}

	const auto &K   = config_.camera_matrix;
	const double fx = K[0];
	const double fy = K[4];
	const double cx = K[2];
	const double cy = K[5];
	if (!std::isfinite(fx) || !std::isfinite(fy) || fx <= 0.0 || fy <= 0.0) {
		throw std::invalid_argument("invalid focal lengths in camera matrix");
	}

	for (const auto value : config_.dist_coeffs) {
		if (!std::isfinite(value)) {
			throw std::invalid_argument("distortion coefficients contain non-finite values");
		}
	}

	if ((config_.use_optimal_new_camera_matrix || config_.crop_to_valid_roi) && map_.src_x.empty()) {
		spdlog::warn("undistort: use_optimal_new_camera_matrix/crop_to_valid_roi are not yet supported in internal path; using original camera matrix");
	}

	DistortionParams d;
	if (!config_.dist_coeffs.empty()) {
		auto get = [&](size_t idx) -> double {
			return idx < config_.dist_coeffs.size() ? config_.dist_coeffs[idx] : 0.0;
		};
		d.k1 = get(0);
		d.k2 = get(1);
		d.p1 = get(2);
		d.p2 = get(3);
		d.k3 = get(4);
		d.k4 = get(5);
		d.k5 = get(6);
		d.k6 = get(7);
		d.s1 = get(8);
		d.s2 = get(9);
		d.s3 = get(10);
		d.s4 = get(11);
	}

	map_.width  = width;
	map_.height = height;
	map_.src_x.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0F);
	map_.src_y.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0.0F);

	for (int v = 0; v < height; ++v) {
		for (int u = 0; u < width; ++u) {
			const double x  = (static_cast<double>(u) - cx) / fx;
			const double y  = (static_cast<double>(v) - cy) / fy;
			const double r2 = x * x + y * y;
			const double r4 = r2 * r2;
			const double r6 = r4 * r2;

			double radial_num = 1.0 + d.k1 * r2 + d.k2 * r4 + d.k3 * r6;
			double radial_den = 1.0 + d.k4 * r2 + d.k5 * r4 + d.k6 * r6;
			if (std::abs(radial_den) < 1e-12) {
				radial_den = 1.0;
			}
			const double radial = radial_num / radial_den;

			const double two_xy = 2.0 * x * y;
			const double x2     = x * x;
			const double y2     = y * y;

			const double delta_x = d.p1 * two_xy + d.p2 * (r2 + 2.0 * x2) + d.s1 * r2 + d.s2 * r4;
			const double delta_y = d.p1 * (r2 + 2.0 * y2) + d.p2 * two_xy + d.s3 * r2 + d.s4 * r4;

			const double xd = x * radial + delta_x;
			const double yd = y * radial + delta_y;

			const float src_x = static_cast<float>(fx * xd + cx);
			const float src_y = static_cast<float>(fy * yd + cy);

			const size_t idx = static_cast<size_t>(v) * static_cast<size_t>(width) + static_cast<size_t>(u);
			map_.src_x[idx]  = src_x;
			map_.src_y[idx]  = src_y;
		}
	}

	map_ready_ = true;
	spdlog::debug("{}", kAttributionNote);
	spdlog::info("undistort map initialized for {}x{}", width, height);
}

void UndistortPass::remap_u8_linear(std::span<const uint8_t> src, std::span<uint8_t> dst, const app::frame_info_t &info) const {
	const int width    = static_cast<int>(info.width);
	const int height   = static_cast<int>(info.height);
	const int channels = static_cast<int>(info.channels);
	const uint8_t *s   = src.data();

	for (int v = 0; v < height; ++v) {
		for (int u = 0; u < width; ++u) {
			const size_t idx = static_cast<size_t>(v) * static_cast<size_t>(width) + static_cast<size_t>(u);
			const float x    = map_.src_x[idx];
			const float y    = map_.src_y[idx];
			for (int c = 0; c < channels; ++c) {
				dst[idx * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
					sample_u8_bilinear(s, width, height, channels, x, y, c);
			}
		}
	}
}

std::span<const uint8_t> UndistortPass::apply(std::span<const uint8_t> input, const app::frame_info_t &info) {
	if (!enabled()) {
		return input;
	}

	if (!supports(info)) {
		const std::string reason = "input format is not supported by undistort pass";
		if (config_.strict_startup) {
			throw std::runtime_error(reason);
		}
		disable(reason);
		return input;
	}

	if (input.size() != expected_buffer_size(info) || input.size() != static_cast<size_t>(info.buffer_size)) {
		const std::string reason = "input buffer size does not match frame metadata";
		if (config_.strict_startup) {
			throw std::runtime_error(reason);
		}
		disable(reason);
		return input;
	}

	if (!map_ready_ || map_.width != static_cast<int>(info.width) || map_.height != static_cast<int>(info.height)) {
		map_ready_         = false;
		attempted_rebuild_ = false;
		try {
			build_map(info);
		} catch (const std::exception &e) {
			if (config_.strict_startup) {
				throw;
			}
			disable(std::string("undistort map initialization failed: ") + e.what());
			return input;
		}
	}

	output_bytes_.assign(input.size(), 0);

	try {
		remap_u8_linear(input, std::span<uint8_t>(output_bytes_.data(), output_bytes_.size()), info);
		attempted_rebuild_ = false;
	} catch (const std::exception &e) {
		if (!attempted_rebuild_) {
			attempted_rebuild_ = true;
			try {
				build_map(info);
				remap_u8_linear(input, std::span<uint8_t>(output_bytes_.data(), output_bytes_.size()), info);
				attempted_rebuild_ = false;
			} catch (const std::exception &inner) {
				if (config_.strict_startup) {
					throw;
				}
				disable(std::string("undistort remap failed after rebuild: ") + inner.what());
				return input;
			}
		} else {
			if (config_.strict_startup) {
				throw;
			}
			disable(std::string("undistort remap failed: ") + e.what());
			return input;
		}
	}

	return std::span<const uint8_t>(output_bytes_.data(), output_bytes_.size());
}

std::optional<UndistortPass> make_undistort_pass(const std::optional<app::PreprocessConfig> &preprocess) {
	if (!preprocess || !preprocess->undistort) {
		return std::nullopt;
	}
	if (!preprocess->undistort->enabled) {
		return std::nullopt;
	}
	return UndistortPass(*preprocess->undistort);
}

}
