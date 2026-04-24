#pragma once

#include "app_config.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <toml++/toml.hpp>

namespace app {

std::string_view to_string(cvmmap::TimestampDomain domain);
cvmmap::TimestampDomain timestamp_domain_from_string(std::string_view s);

namespace config_detail {

std::string normalize_pipeline_string(std::string pipeline);
std::array<double, 9> parse_camera_matrix(
	const toml::node_view<toml::node> &node);
std::vector<double> parse_dist_coeffs(const toml::node_view<toml::node> &node);
std::string normalize_ascii_lower(std::string value);
std::string trim_ascii_spaces(std::string value);
std::vector<std::string> parse_playlist_paths(
	const toml::node_view<toml::node> &node,
	std::string_view field_name);
bool parse_bool_field(
	const toml::node_view<toml::node> &node,
	std::string_view field_name,
	bool default_value);

std::string validate_ipc_prefix(std::string prefix);
void validate_ipc_config(const Config &config);
void validate_undistort_config(const UndistortConfig &cfg);
void validate_zed_body_tracking_config(
	const ZedConfig::BodyTrackingConfig &cfg);
void validate_zed_recording_config(const ZedConfig::RecordingConfig &cfg);
void validate_zed_runtime_config(const ZedConfig &cfg);

bool is_zed_network_stream_mode(std::string_view mode);
bool is_zed_svo_stream_mode(std::string_view mode);
bool is_zed_local_stream_mode(std::string_view mode);
bool is_valid_zed_stream_mode(std::string_view mode);
std::string canonical_zed_stream_mode(std::string_view mode);
std::string validate_and_canonicalize_zed_resolution(
	std::string_view resolution);
std::string validate_and_canonicalize_zed_depth_mode(
	std::string_view depth_mode);
std::string canonicalize_zed_body_tracking_model(
	std::string_view detection_model);
std::string canonicalize_zed_body_format(std::string_view body_format);
std::string canonicalize_zed_body_selection(std::string_view body_selection);
std::string canonicalize_zed_coordinate_system(
	std::string_view coordinate_system);
std::string canonicalize_zed_body_reference_frame(
	std::string_view reference_frame);
std::string canonicalize_zed_recording_compression_mode(
	std::string_view compression_mode);

toml::table load_effective_config_table(const std::filesystem::path &path);

} // namespace config_detail
} // namespace app
