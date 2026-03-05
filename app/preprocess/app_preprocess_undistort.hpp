#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "app_config.hpp"
#include "app_metadata_models.hpp"

namespace app::preprocess {

class UndistortPass {
public:
	explicit UndistortPass(app::UndistortConfig config);

	[[nodiscard]]
	bool enabled() const;

	[[nodiscard]]
	std::span<const uint8_t> apply(std::span<const uint8_t> input, const app::frame_info_t &info);

private:
	struct MapGrid {
		int width{0};
		int height{0};
		std::vector<float> src_x;
		std::vector<float> src_y;
	};

	void build_map(const app::frame_info_t &info);
	[[nodiscard]]
	bool supports(const app::frame_info_t &info) const;
	[[nodiscard]]
	size_t expected_buffer_size(const app::frame_info_t &info) const;
	void disable(std::string reason);
	void remap_u8_linear(std::span<const uint8_t> src, std::span<uint8_t> dst, const app::frame_info_t &info) const;

	app::UndistortConfig config_;
	bool active_{true};
	bool map_ready_{false};
	bool attempted_rebuild_{false};
	std::string disabled_reason_;
	MapGrid map_;
	std::vector<uint8_t> output_bytes_;
};

std::optional<UndistortPass> make_undistort_pass(const std::optional<app::PreprocessConfig> &preprocess);

}
