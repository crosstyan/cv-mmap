#include "app_config_internal.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace app::config_detail {

std::string normalize_pipeline_string(std::string pipeline) {
	pipeline.erase(std::remove(pipeline.begin(), pipeline.end(), '\r'), pipeline.end());
	std::replace(pipeline.begin(), pipeline.end(), '\n', ' ');
	return pipeline;
}

std::array<double, 9> parse_camera_matrix(
	const toml::node_view<toml::node> &node) {
	auto arr = node.as_array();
	if (!arr) {
		throw std::invalid_argument("preprocess.undistort.camera_matrix must be an array");
	}
	if (arr->size() != 9) {
		throw std::invalid_argument("preprocess.undistort.camera_matrix must have exactly 9 values");
	}

	std::array<double, 9> values{};
	for (size_t i = 0; i < 9; ++i) {
		auto value = (*arr)[i].value<double>();
		if (!value) {
			throw std::invalid_argument("preprocess.undistort.camera_matrix must contain only numeric values");
		}
		values[i] = *value;
	}
	return values;
}

std::vector<double> parse_dist_coeffs(
	const toml::node_view<toml::node> &node) {
	auto arr = node.as_array();
	if (!arr) {
		throw std::invalid_argument("preprocess.undistort.dist_coeffs must be an array");
	}
	std::vector<double> values;
	values.reserve(arr->size());
	for (const auto &entry : *arr) {
		auto value = entry.value<double>();
		if (!value) {
			throw std::invalid_argument("preprocess.undistort.dist_coeffs must contain only numeric values");
		}
		values.push_back(*value);
	}
	return values;
}

std::string normalize_ascii_lower(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return value;
}

std::string trim_ascii_spaces(std::string value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) {
		return "";
	}
	const auto last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

std::vector<std::string> parse_playlist_paths(
	const toml::node_view<toml::node> &node,
	const std::string_view field_name) {
	auto arr = node.as_array();
	if (!arr) {
		throw std::invalid_argument(std::string(field_name) + " must be an array of strings");
	}

	std::vector<std::string> paths{};
	paths.reserve(arr->size());
	for (const auto &entry : *arr) {
		auto path = entry.value<std::string>();
		if (!path) {
			throw std::invalid_argument(std::string(field_name) + " must contain only strings");
		}
		auto trimmed = trim_ascii_spaces(*path);
		if (trimmed.empty()) {
			throw std::invalid_argument(std::string(field_name) + " must not contain empty paths");
		}
		paths.push_back(std::move(trimmed));
	}
	if (paths.empty()) {
		throw std::invalid_argument(std::string(field_name) + " must not be empty");
	}
	return paths;
}

bool parse_bool_field(
	const toml::node_view<toml::node> &node,
	const std::string_view field_name,
	const bool default_value) {
	if (!node) {
		return default_value;
	}
	if (auto value = node.value<bool>(); value) {
		return *value;
	}
	throw std::invalid_argument(std::string(field_name) + " must be boolean");
}

bool is_zed_network_stream_mode(const std::string_view mode) {
	const auto normalized = normalize_ascii_lower(std::string(mode));
	return normalized == "network" || normalized == "ethernet" || normalized == "stream";
}

bool is_zed_svo_stream_mode(const std::string_view mode) {
	const auto normalized = normalize_ascii_lower(std::string(mode));
	return normalized == "svo";
}

bool is_zed_local_stream_mode(const std::string_view mode) {
	const auto normalized = normalize_ascii_lower(std::string(mode));
	return normalized == "local" || normalized == "usb" || normalized == "device" || normalized == "auto";
}

bool is_valid_zed_stream_mode(const std::string_view mode) {
	return is_zed_network_stream_mode(mode) ||
		   is_zed_local_stream_mode(mode) ||
		   is_zed_svo_stream_mode(mode);
}

std::string canonical_zed_stream_mode(const std::string_view mode) {
	if (is_zed_svo_stream_mode(mode)) {
		return "svo";
	}
	return is_zed_network_stream_mode(mode) ? "network" : "local";
}

std::string validate_and_canonicalize_zed_resolution(
	const std::string_view resolution) {
	const auto normalized = normalize_ascii_lower(std::string(resolution));

	if (normalized == "2k") {
		return "HD2K";
	}
	if (normalized == "1080p" || normalized == "fhd") {
		return "HD1080";
	}
	if (normalized == "720p" || normalized == "hd") {
		return "HD720";
	}

	static const std::unordered_set<std::string> valid_resolutions = {
		"hd2k", "hd1200", "hd1080", "hd720", "svga", "vga", "auto"};

	if (valid_resolutions.find(normalized) != valid_resolutions.end()) {
		std::string result = normalized;
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return result;
	}

	throw std::invalid_argument(
		"invalid zed.resolution: '" + std::string(resolution) +
		"'. Allowed values: HD2K, HD1200, HD1080, HD720, SVGA, VGA, AUTO "
		"(aliases: 2k, 1080p, fhd, 720p, hd)");
}

std::string validate_and_canonicalize_zed_depth_mode(
	const std::string_view depth_mode) {
	auto normalized = normalize_ascii_lower(std::string(depth_mode));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	static const std::unordered_set<std::string> valid_depth_modes = {
		"none", "neural", "neural_light", "neural_plus"};

	if (valid_depth_modes.find(normalized) != valid_depth_modes.end()) {
		std::string result = normalized;
		std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
			return static_cast<char>(std::toupper(c));
		});
		return result;
	}

	throw std::invalid_argument(
		"invalid zed.depth_mode: '" + std::string(depth_mode) +
		"'. Allowed values: NONE, NEURAL, NEURAL_LIGHT, NEURAL_PLUS "
		"(aliases: neural light, neural-light, neural plus, neural-plus)");
}

std::string canonicalize_zed_body_tracking_model(
	const std::string_view detection_model) {
	auto normalized = normalize_ascii_lower(trim_ascii_spaces(std::string(detection_model)));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	if (normalized == "fast" || normalized == "human_body_fast") {
		return "HUMAN_BODY_FAST";
	}
	if (normalized == "medium" || normalized == "human_body_medium") {
		return "HUMAN_BODY_MEDIUM";
	}
	if (normalized == "accurate" || normalized == "human_body_accurate") {
		return "HUMAN_BODY_ACCURATE";
	}

	throw std::invalid_argument(
		"invalid zed.body_tracking.detection_model: '" + std::string(detection_model) +
		"'. Allowed values: HUMAN_BODY_FAST, HUMAN_BODY_MEDIUM, HUMAN_BODY_ACCURATE");
}

std::string canonicalize_zed_body_format(const std::string_view body_format) {
	auto normalized = normalize_ascii_lower(trim_ascii_spaces(std::string(body_format)));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	if (normalized == "body_18" || normalized == "18") {
		return "BODY_18";
	}
	if (normalized == "body_34" || normalized == "34") {
		return "BODY_34";
	}
	if (normalized == "body_38" || normalized == "38") {
		return "BODY_38";
	}

	throw std::invalid_argument(
		"invalid zed.body_tracking.body_format: '" + std::string(body_format) +
		"'. Allowed values: BODY_18, BODY_34, BODY_38");
}

std::string canonicalize_zed_body_selection(
	const std::string_view body_selection) {
	auto normalized = normalize_ascii_lower(trim_ascii_spaces(std::string(body_selection)));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	if (normalized == "full") {
		return "FULL";
	}
	if (normalized == "upper_body" || normalized == "upper") {
		return "UPPER_BODY";
	}

	throw std::invalid_argument(
		"invalid zed.body_tracking.body_selection: '" + std::string(body_selection) +
		"'. Allowed values: FULL, UPPER_BODY");
}

std::string canonicalize_zed_coordinate_system(
	const std::string_view coordinate_system) {
	auto normalized = normalize_ascii_lower(trim_ascii_spaces(std::string(coordinate_system)));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	if (normalized == "image") {
		return "IMAGE";
	}
	if (normalized == "right_handed_y_up" || normalized == "y_up") {
		return "RIGHT_HANDED_Y_UP";
	}

	throw std::invalid_argument(
		"invalid zed.coordinate_system: '" + std::string(coordinate_system) +
		"'. Allowed values: IMAGE, RIGHT_HANDED_Y_UP");
}

std::string canonicalize_zed_body_reference_frame(
	const std::string_view reference_frame) {
	auto normalized = normalize_ascii_lower(trim_ascii_spaces(std::string(reference_frame)));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	if (normalized == "camera") {
		return "CAMERA";
	}
	if (normalized == "world") {
		return "WORLD";
	}

	throw std::invalid_argument(
		"invalid zed.body_tracking.reference_frame: '" + std::string(reference_frame) +
		"'. Allowed values: CAMERA, WORLD");
}

std::string canonicalize_zed_recording_compression_mode(
	const std::string_view compression_mode) {
	auto normalized = normalize_ascii_lower(trim_ascii_spaces(std::string(compression_mode)));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');

	if (normalized == "lossless") {
		return "LOSSLESS";
	}
	if (normalized == "h264") {
		return "H264";
	}
	if (normalized == "h265") {
		return "H265";
	}
	if (normalized == "h264_lossless") {
		return "H264_LOSSLESS";
	}
	if (normalized == "h265_lossless") {
		return "H265_LOSSLESS";
	}

	throw std::invalid_argument(
		"invalid zed.recording.compression_mode: '" + std::string(compression_mode) +
		"'. Allowed values: LOSSLESS, H264, H265, H264_LOSSLESS, H265_LOSSLESS");
}

} // namespace app::config_detail
