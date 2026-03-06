#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <memory>
#include <vector>
#include "app_config.hpp"
#include "app_metadata_models.hpp"

namespace app::preprocess {

class UndistortPass {
public:
	explicit UndistortPass(app::UndistortConfig config);
	~UndistortPass();

	[[nodiscard]]
	bool enabled() const;

	[[nodiscard]]
	std::span<const uint8_t> apply(std::span<const uint8_t> input, const app::frame_info_t &info);

private:
	struct MapGrid {
		int width{0};
		int height{0};
		uint8_t channels{0};
		app::Depth depth{app::Depth::U8};
		app::PixelFormat pixel_format{app::PixelFormat::BGR};
		std::vector<uint32_t> base_offset;
		std::vector<uint16_t> w00;
		std::vector<uint16_t> w01;
		std::vector<uint16_t> w10;
		std::vector<uint16_t> w11;
		std::vector<uint8_t> valid;
	};

	void build_map(const app::frame_info_t &info);
	[[nodiscard]]
	bool supports(const app::frame_info_t &info) const;
	[[nodiscard]]
	size_t expected_buffer_size(const app::frame_info_t &info) const;
	void disable(std::string reason);
	void remap_u8_linear(std::span<const uint8_t> src, std::span<uint8_t> dst, const app::frame_info_t &info) const;
	void ensure_output_buffer(size_t size);
	void build_internal_map(const app::frame_info_t &info);
	[[nodiscard]]
	bool should_use_opencv_backend() const;
	void warn_about_unsupported_options();

	struct OpenCVState;
	void build_opencv_state(const app::frame_info_t &info);
	void remap_opencv(std::span<const uint8_t> src, std::span<uint8_t> dst, const app::frame_info_t &info);

	app::UndistortConfig config_;
	bool active_{true};
	bool map_ready_{false};
	bool attempted_rebuild_{false};
	bool warned_internal_camera_matrix_options_{false};
	bool warned_crop_to_valid_roi_{false};
	std::string disabled_reason_;
	MapGrid map_;
	std::vector<uint8_t> output_bytes_;
	std::shared_ptr<OpenCVState> opencv_state_;
};

std::optional<UndistortPass> make_undistort_pass(const std::optional<app::PreprocessConfig> &preprocess);

}
