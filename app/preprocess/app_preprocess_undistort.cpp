#include "app_preprocess_undistort.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <spdlog/spdlog.h>

#ifdef WITH_PREPROCESS_UNDISTORT_OPENCV
#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/core.hpp>
#endif

namespace app::preprocess {
namespace {
	constexpr int kBilinearWeightShift = 8;
	constexpr int kBilinearWeightScale = 1 << kBilinearWeightShift;

	const char *kAttributionNote =
		"Undistortion equations adapted from OpenCV calib3d implementation (Brown-Conrady mapping logic), "
		"implemented here without linking OpenCV for preprocess path. "
		"Internal cached bilinear remap layout also follows the same map-then-remap structure used by OpenCV's undistort/remap pipeline. "
		"References: opencv/modules/calib3d/src/undistort.dispatch.cpp, opencv/modules/calib3d/src/distortion_model.hpp, opencv/modules/imgproc/src/imgwarp.cpp";

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

	struct TiltMatrix {
		double v[9]{1.0, 0.0, 0.0,
					0.0, 1.0, 0.0,
					0.0, 0.0, 1.0};
	};

	[[nodiscard]]
	TiltMatrix compute_tilt_projection_matrix(const double tau_x, const double tau_y) {
		const double c_tau_x = std::cos(tau_x);
		const double s_tau_x = std::sin(tau_x);
		const double c_tau_y = std::cos(tau_y);
		const double s_tau_y = std::sin(tau_y);

		const double rot_xy_00 = c_tau_y;
		const double rot_xy_01 = s_tau_y * s_tau_x;
		const double rot_xy_02 = -s_tau_y * c_tau_x;
		const double rot_xy_10 = 0.0;
		const double rot_xy_11 = c_tau_x;
		const double rot_xy_12 = s_tau_x;
		const double rot_xy_20 = s_tau_y;
		const double rot_xy_21 = -c_tau_y * s_tau_x;
		const double rot_xy_22 = c_tau_y * c_tau_x;

		TiltMatrix mat;
		mat.v[0] = rot_xy_22 * rot_xy_00 - rot_xy_02 * rot_xy_20;
		mat.v[1] = rot_xy_22 * rot_xy_01 - rot_xy_02 * rot_xy_21;
		mat.v[2] = 0.0;
		mat.v[3] = -rot_xy_12 * rot_xy_20;
		mat.v[4] = rot_xy_22 * rot_xy_11 - rot_xy_12 * rot_xy_21;
		mat.v[5] = 0.0;
		mat.v[6] = rot_xy_20;
		mat.v[7] = rot_xy_21;
		mat.v[8] = rot_xy_22;
		return mat;
	}

	[[nodiscard]]
	uint8_t interpolate_weighted_u8(const uint8_t *src, const uint32_t base, const uint32_t stride, const int channel,
									const uint16_t w00, const uint16_t w01, const uint16_t w10, const uint16_t w11) {
		const uint32_t top_left     = base + static_cast<uint32_t>(channel);
		const uint32_t top_right    = top_left + stride;
		const uint32_t bottom_left  = base + static_cast<uint32_t>(channel + stride);
		const uint32_t bottom_right = bottom_left + stride;
		const uint32_t accum =
			w00 * static_cast<uint32_t>(src[top_left]) +
			w01 * static_cast<uint32_t>(src[top_right]) +
			w10 * static_cast<uint32_t>(src[bottom_left]) +
			w11 * static_cast<uint32_t>(src[bottom_right]);
		return static_cast<uint8_t>((accum + (kBilinearWeightScale / 2U)) >> kBilinearWeightShift);
	}

#ifdef WITH_PREPROCESS_UNDISTORT_OPENCV
	[[nodiscard]]
	int cv_type_for_frame_info(const app::frame_info_t &info) {
		if (info.depth != app::Depth::U8) {
			throw std::invalid_argument("OpenCV undistort supports only U8 input in preprocess path");
		}

		switch (info.channels) {
		case 1:
			return CV_MAKETYPE(CV_8U, 1);
		case 3:
			return CV_MAKETYPE(CV_8U, 3);
		case 4:
			return CV_MAKETYPE(CV_8U, 4);
		default:
			throw std::invalid_argument("OpenCV undistort supports only 1, 3, or 4 channels");
		}
	}
#endif
}

#ifdef WITH_PREPROCESS_UNDISTORT_OPENCV
struct UndistortPass::OpenCVState {
	int width{0};
	int height{0};
	int cv_type{0};
	cv::Mat camera_matrix;
	cv::Mat dist_coeffs;
	cv::Mat new_camera_matrix;
	cv::Rect valid_roi;
	cv::Mat map1;
	cv::Mat map2;
};
#else
struct UndistortPass::OpenCVState {};
#endif

UndistortPass::UndistortPass(app::UndistortConfig config) : config_(std::move(config)) {}

UndistortPass::~UndistortPass() = default;

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

namespace {
	[[nodiscard]]
	constexpr bool has_internal_undistort_fallback() {
#ifdef WITH_PREPROCESS_UNDISTORT_INTERNAL_FALLBACK
		return true;
#else
		return false;
#endif
	}
}

void UndistortPass::disable(std::string reason) {
	if (!active_) {
		return;
	}
	active_          = false;
	disabled_reason_ = std::move(reason);
	spdlog::warn("undistort disabled: {}", disabled_reason_);
}

bool UndistortPass::should_use_opencv_backend() const {
#ifdef WITH_PREPROCESS_UNDISTORT_OPENCV
	return true;
#else
	return false;
#endif
}

void UndistortPass::warn_about_unsupported_options() {
	if (config_.crop_to_valid_roi && !warned_crop_to_valid_roi_) {
		spdlog::warn("undistort: crop_to_valid_roi is not supported by the current preprocess output contract; keeping original frame dimensions");
		warned_crop_to_valid_roi_ = true;
	}

	if (!should_use_opencv_backend()) {
		if (!warned_internal_camera_matrix_options_) {
			spdlog::warn("undistort: using internal fallback algorithm; repeated-frame performance will be significantly slower than the OpenCV remap path");
			warned_internal_camera_matrix_options_ = true;
		}
		if (config_.use_optimal_new_camera_matrix) {
			spdlog::warn("undistort: use_optimal_new_camera_matrix is not supported in the internal fallback path; using original camera matrix");
		}
	}
}

#ifdef WITH_PREPROCESS_UNDISTORT_INTERNAL_FALLBACK
void UndistortPass::build_internal_map(const app::frame_info_t &info) {
	const int width  = static_cast<int>(info.width);
	const int height = static_cast<int>(info.height);
	if (width <= 0 || height <= 0) {
		throw std::invalid_argument("invalid frame dimensions");
	}

	const auto &K       = config_.camera_matrix;
	const double fx     = K[0];
	const double fy     = K[4];
	const double cx     = K[2];
	const double cy     = K[5];
	const double tau_x  = config_.dist_coeffs.size() >= 14 ? config_.dist_coeffs[12] : 0.0;
	const double tau_y  = config_.dist_coeffs.size() >= 14 ? config_.dist_coeffs[13] : 0.0;
	const auto mat_tilt = compute_tilt_projection_matrix(tau_x, tau_y);
	if (!std::isfinite(fx) || !std::isfinite(fy) || fx <= 0.0 || fy <= 0.0) {
		throw std::invalid_argument("invalid focal lengths in camera matrix");
	}

	for (const auto value : config_.dist_coeffs) {
		if (!std::isfinite(value)) {
			throw std::invalid_argument("distortion coefficients contain non-finite values");
		}
	}

	warn_about_unsupported_options();

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

	map_.width            = width;
	map_.height           = height;
	map_.channels         = info.channels;
	map_.depth            = info.depth;
	map_.pixel_format     = info.pixel_format;
	const size_t map_size = static_cast<size_t>(width) * static_cast<size_t>(height);
	map_.base_offset.assign(map_size, 0U);
	map_.w00.assign(map_size, 0U);
	map_.w01.assign(map_size, 0U);
	map_.w10.assign(map_size, 0U);
	map_.w11.assign(map_size, 0U);
	map_.valid.assign(map_size, 0U);
	const uint32_t row_stride = static_cast<uint32_t>(width) * static_cast<uint32_t>(info.channels);

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

			const double xd          = x * radial + delta_x;
			const double yd          = y * radial + delta_y;
			const double tilt_x      = mat_tilt.v[0] * xd + mat_tilt.v[1] * yd + mat_tilt.v[2];
			const double tilt_y      = mat_tilt.v[3] * xd + mat_tilt.v[4] * yd + mat_tilt.v[5];
			const double tilt_w      = mat_tilt.v[6] * xd + mat_tilt.v[7] * yd + mat_tilt.v[8];
			const double inv_proj    = std::abs(tilt_w) > 1e-12 ? 1.0 / tilt_w : 1.0;
			const double projected_x = tilt_x * inv_proj;
			const double projected_y = tilt_y * inv_proj;

			const double src_x = fx * projected_x + cx;
			const double src_y = fy * projected_y + cy;

			const size_t idx = static_cast<size_t>(v) * static_cast<size_t>(width) + static_cast<size_t>(u);
			if (src_x < 0.0 || src_y < 0.0 || src_x >= static_cast<double>(width - 1) || src_y >= static_cast<double>(height - 1)) {
				continue;
			}

			const int x0          = static_cast<int>(src_x);
			const int y0          = static_cast<int>(src_y);
			const double frac_x   = src_x - static_cast<double>(x0);
			const double frac_y   = src_y - static_cast<double>(y0);
			const int wx          = static_cast<int>(std::clamp(std::lround(frac_x * kBilinearWeightScale), 0L, static_cast<long>(kBilinearWeightScale)));
			const int wy          = static_cast<int>(std::clamp(std::lround(frac_y * kBilinearWeightScale), 0L, static_cast<long>(kBilinearWeightScale)));
			const uint16_t w00    = static_cast<uint16_t>(((kBilinearWeightScale - wx) * (kBilinearWeightScale - wy)) >> kBilinearWeightShift);
			const uint16_t w01    = static_cast<uint16_t>((wx * (kBilinearWeightScale - wy)) >> kBilinearWeightShift);
			const uint16_t w10    = static_cast<uint16_t>(((kBilinearWeightScale - wx) * wy) >> kBilinearWeightShift);
			const uint16_t w11    = static_cast<uint16_t>(kBilinearWeightScale - w00 - w01 - w10);
			map_.base_offset[idx] = static_cast<uint32_t>(y0) * row_stride + static_cast<uint32_t>(x0) * static_cast<uint32_t>(info.channels);
			map_.w00[idx]         = w00;
			map_.w01[idx]         = w01;
			map_.w10[idx]         = w10;
			map_.w11[idx]         = w11;
			map_.valid[idx]       = 1U;
		}
	}

	map_ready_ = true;
	spdlog::debug("{}", kAttributionNote);
	spdlog::info("undistort map initialized for {}x{}", width, height);
}
#else
void UndistortPass::build_internal_map(const app::frame_info_t &) {
	throw std::invalid_argument("internal undistort fallback is not compiled in this build");
}
#endif

void UndistortPass::build_map(const app::frame_info_t &info) {
	if (should_use_opencv_backend()) {
		build_opencv_state(info);
		map_ready_ = true;
		return;
	}

	if (!has_internal_undistort_fallback()) {
		throw std::invalid_argument("undistort is enabled but no implementation is available in this build");
	}

	build_internal_map(info);
}

#ifdef WITH_PREPROCESS_UNDISTORT_INTERNAL_FALLBACK
void UndistortPass::remap_u8_linear(std::span<const uint8_t> src, std::span<uint8_t> dst, const app::frame_info_t &info) const {
	const int width           = static_cast<int>(info.width);
	const int height          = static_cast<int>(info.height);
	const int channels        = static_cast<int>(info.channels);
	const uint8_t *s          = src.data();
	const uint32_t row_stride = static_cast<uint32_t>(width) * static_cast<uint32_t>(channels);

	for (int v = 0; v < height; ++v) {
		for (int u = 0; u < width; ++u) {
			const size_t idx      = static_cast<size_t>(v) * static_cast<size_t>(width) + static_cast<size_t>(u);
			const size_t dst_base = idx * static_cast<size_t>(channels);
			if (!map_.valid[idx]) {
				for (int c = 0; c < channels; ++c) {
					dst[dst_base + static_cast<size_t>(c)] = 0;
				}
				continue;
			}

			const uint32_t base = map_.base_offset[idx];
			const uint16_t w00  = map_.w00[idx];
			const uint16_t w01  = map_.w01[idx];
			const uint16_t w10  = map_.w10[idx];
			const uint16_t w11  = map_.w11[idx];
			for (int c = 0; c < channels; ++c) {
				dst[dst_base + static_cast<size_t>(c)] =
					interpolate_weighted_u8(s, base, row_stride, c, w00, w01, w10, w11);
			}
		}
	}
}
#else
void UndistortPass::remap_u8_linear(std::span<const uint8_t>, std::span<uint8_t>, const app::frame_info_t &) const {
	throw std::invalid_argument("internal undistort fallback is not compiled in this build");
}
#endif

void UndistortPass::ensure_output_buffer(size_t size) {
	if (output_bytes_.size() != size) {
		output_bytes_.resize(size);
	}
}

#ifdef WITH_PREPROCESS_UNDISTORT_OPENCV
void UndistortPass::build_opencv_state(const app::frame_info_t &info) {
	warn_about_unsupported_options();
	const int width  = static_cast<int>(info.width);
	const int height = static_cast<int>(info.height);
	if (width <= 0 || height <= 0) {
		throw std::invalid_argument("invalid frame dimensions");
	}

	if (!opencv_state_) {
		opencv_state_ = std::make_shared<OpenCVState>();
	}

	auto &state         = *opencv_state_;
	state.width         = width;
	state.height        = height;
	state.cv_type       = cv_type_for_frame_info(info);
	state.camera_matrix = (cv::Mat_<double>(3, 3) << config_.camera_matrix[0], config_.camera_matrix[1], config_.camera_matrix[2],
						   config_.camera_matrix[3], config_.camera_matrix[4], config_.camera_matrix[5],
						   config_.camera_matrix[6], config_.camera_matrix[7], config_.camera_matrix[8]);
	state.dist_coeffs   = cv::Mat(static_cast<int>(config_.dist_coeffs.size()), 1, CV_64F);
	for (int i = 0; i < state.dist_coeffs.rows; ++i) {
		state.dist_coeffs.at<double>(i, 0) = config_.dist_coeffs[static_cast<size_t>(i)];
	}

	const cv::Size size(width, height);
	if (config_.use_optimal_new_camera_matrix) {
		state.new_camera_matrix = cv::getOptimalNewCameraMatrix(
			state.camera_matrix,
			state.dist_coeffs,
			size,
			config_.alpha,
			size,
			&state.valid_roi,
			false);
	} else {
		state.new_camera_matrix = state.camera_matrix.clone();
		state.valid_roi         = cv::Rect(0, 0, width, height);
	}

	cv::initUndistortRectifyMap(
		state.camera_matrix,
		state.dist_coeffs,
		cv::Mat(),
		state.new_camera_matrix,
		size,
		CV_16SC2,
		state.map1,
		state.map2);

	spdlog::info("undistort OpenCV map initialized for {}x{}", width, height);
}

void UndistortPass::remap_opencv(std::span<const uint8_t> src, std::span<uint8_t> dst, const app::frame_info_t &info) {
	if (!opencv_state_) {
		throw std::logic_error("OpenCV undistort state is not initialized");
	}

	auto &state = *opencv_state_;
	cv::Mat src_mat(static_cast<int>(info.height), static_cast<int>(info.width), state.cv_type, const_cast<uint8_t *>(src.data()));
	cv::Mat dst_mat(static_cast<int>(info.height), static_cast<int>(info.width), state.cv_type, dst.data());
	cv::remap(src_mat, dst_mat, state.map1, state.map2, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
}
#else
void UndistortPass::build_opencv_state(const app::frame_info_t &) {
	throw std::invalid_argument("OpenCV undistort backend was requested but is not available in this build");
}

void UndistortPass::remap_opencv(std::span<const uint8_t>, std::span<uint8_t>, const app::frame_info_t &) {
	throw std::invalid_argument("OpenCV undistort backend was requested but is not available in this build");
}
#endif

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

	if (!map_ready_ ||
		map_.width != static_cast<int>(info.width) ||
		map_.height != static_cast<int>(info.height) ||
		map_.channels != info.channels ||
		map_.depth != info.depth ||
		map_.pixel_format != info.pixel_format) {
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

	ensure_output_buffer(input.size());

	try {
		auto output = std::span<uint8_t>(output_bytes_.data(), output_bytes_.size());
		if (should_use_opencv_backend()) {
			remap_opencv(input, output, info);
		} else {
			remap_u8_linear(input, output, info);
		}
		attempted_rebuild_ = false;
	} catch (const std::exception &e) {
		if (!attempted_rebuild_) {
			attempted_rebuild_ = true;
			try {
				build_map(info);
				auto output = std::span<uint8_t>(output_bytes_.data(), output_bytes_.size());
				if (should_use_opencv_backend()) {
					remap_opencv(input, output, info);
				} else {
					remap_u8_linear(input, output, info);
				}
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
	return std::optional<UndistortPass>(std::in_place, *preprocess->undistort);
}

}
