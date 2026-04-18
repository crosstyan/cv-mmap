#include "app_config.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

class TempDir {
public:
	TempDir() {
		const auto unique_suffix = std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count());
		path_ = std::filesystem::temp_directory_path() /
				("cvmmap_app_config_tests_" + unique_suffix);
		std::filesystem::create_directories(path_);
	}

	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(path_, ec);
	}

	[[nodiscard]] const std::filesystem::path &path() const {
		return path_;
	}

private:
	std::filesystem::path path_;
};

void write_file(const std::filesystem::path &path, std::string_view content) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream ofs(path);
	if (!ofs) {
		throw std::runtime_error("failed to open fixture file for writing: " + path.string());
	}
		
	ofs << content;
	if (!ofs) {
		throw std::runtime_error("failed to write fixture file: " + path.string());
	}
}

void expect(const bool condition, std::string_view message) {
	if (!condition) {
		throw std::runtime_error(std::string(message));
	}
}

void expect_throws_contains(const std::function<void()> &fn, std::string_view needle) {
	try {
		fn();
	} catch (const std::exception &e) {
		if (std::string_view(e.what()).find(needle) != std::string_view::npos) {
			return;
		}
		throw std::runtime_error(
			"expected exception containing '" + std::string(needle) +
			"', got '" + std::string(e.what()) + "'");
	}

	throw std::runtime_error("expected exception but none was thrown");
}

std::string base_config_toml() {
	return R"(name = "zed-base"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[nats]
enabled = true
url = "nats://localhost:4222"

[video]
backend = "zed"
finite_stream_ending_behavior = "loop"

[zed]
stream_mode = "local"
index = 0
resolution = "AUTO"
fps = 30
depth_mode = "NONE"
publish_confidence = true
svo_real_time_mode = true
depth_stabilization = 30
coordinate_system = "IMAGE"
open_timeout_ms = 10000
warmup_frames = 15
max_consecutive_failures = 30
reconnect_interval_ms = 1000
reconnect = true
left_pixel_format = "bgr8"

[zed.recording]
compression_mode = "H265"
bitrate = 0
target_framerate = 0
transcode_streaming_input = false

[preprocess.undistort]
enabled = false
model = "pinhole"
camera_matrix = [1000.0, 0.0, 640.0, 0.0, 1000.0, 360.0, 0.0, 0.0, 1.0]
dist_coeffs = [0.0, 1.0, 2.0, 3.0, 4.0]
use_optimal_new_camera_matrix = true
alpha = 0.0
crop_to_valid_roi = false
strict_startup = false
)";
}

struct ZedFixture {
	TempDir dir;
	std::filesystem::path base_path;
	std::filesystem::path derived_path;
	std::filesystem::path array_override_path;
	std::filesystem::path absolute_parent_path;

	ZedFixture()
		: base_path(dir.path() / "base.toml"),
		  derived_path(dir.path() / "instances" / "zed2.toml"),
		  array_override_path(dir.path() / "instances" / "zed-array.toml"),
		  absolute_parent_path(dir.path() / "instances" / "zed-absolute.toml") {
		write_file(base_path, base_config_toml());
		write_file(
			derived_path,
			R"(extends = "../base.toml"
		name = "zed2"

		[zed]
		index = 1
		)"
		);
		write_file(
			array_override_path,
			R"(extends = "../base.toml"
		name = "zed-array"

		[preprocess.undistort]
		dist_coeffs = [9.0, 8.0, 7.0, 6.0]
		)"
		);
		write_file(
			absolute_parent_path,
			"extends = \"" + base_path.string() + "\"\n"
			"name = \"zed-absolute\"\n\n"
			"[zed]\n"
			"index = 3\n"
		);
	}
};

void test_loads_base_file_directly() {
	ZedFixture fixture;
	const auto config = app::Config::from_toml(fixture.base_path);

	expect(config.name == "zed-base", "base config should keep its name");
	expect(config.video.backend == app::BackendType::ZED, "base config should parse zed backend");
	expect(config.zed.has_value(), "base config should populate zed section");
	expect(config.zed->index.has_value() && *config.zed->index == 0, "base config should parse zed.index");
}

void test_loads_derived_file_and_inherits_unspecified_fields() {
	ZedFixture fixture;
	const auto config = app::Config::from_toml(fixture.derived_path);

	expect(config.zed.has_value(), "derived config should populate zed section");
	expect(config.zed->resolution == "AUTO", "derived config should inherit zed.resolution");
	expect(config.zed->fps == 30, "derived config should inherit zed.fps");
	expect(config.preprocess.has_value(), "derived config should inherit preprocess section");
	expect(config.preprocess->undistort.has_value(), "derived config should inherit preprocess.undistort section");
	expect(config.preprocess->undistort->dist_coeffs.size() == 5, "derived config should inherit array values when not overridden");
}

void test_derived_file_overrides_name() {
	ZedFixture fixture;
	const auto config = app::Config::from_toml(fixture.derived_path);

	expect(config.name == "zed2", "derived config should override top-level name");
}

void test_derived_file_overrides_zed_index() {
	ZedFixture fixture;
	const auto config = app::Config::from_toml(fixture.derived_path);

	expect(config.zed.has_value(), "derived config should populate zed section");
	expect(config.zed->index.has_value() && *config.zed->index == 1, "derived config should override zed.index");
}

void test_nested_table_override_leaves_unrelated_base_keys_intact() {
	ZedFixture fixture;
	const auto config = app::Config::from_toml(fixture.derived_path);

	expect(config.zed.has_value(), "derived config should populate zed section");
	expect(config.zed->recording.compression_mode == "H265", "nested override should preserve sibling nested keys");
	expect(config.ipc.name_space == "cvmmap", "nested override should preserve top-level inherited tables");
}

void test_array_override_replaces_parent_array() {
	ZedFixture fixture;
	const auto config = app::Config::from_toml(fixture.array_override_path);

	expect(config.preprocess.has_value(), "array override config should populate preprocess section");
	expect(config.preprocess->undistort.has_value(), "array override config should populate preprocess.undistort");
	const auto &dist_coeffs = config.preprocess->undistort->dist_coeffs;
	expect(dist_coeffs.size() == 4, "child array override should replace parent array length");
	expect(dist_coeffs[0] == 9.0 && dist_coeffs[3] == 6.0, "child array override should replace parent array values");
	expect(config.preprocess->undistort->camera_matrix[0] == 1000.0, "array override should preserve sibling scalar and array fields");
}

void test_absolute_parent_path_is_supported() {
	ZedFixture fixture;
	const auto config = app::Config::from_toml(fixture.absolute_parent_path);

	expect(config.name == "zed-absolute", "absolute extends path should allow overriding name");
	expect(config.zed.has_value(), "absolute extends path should inherit zed section");
	expect(config.zed->index.has_value() && *config.zed->index == 3, "absolute extends path should override zed.index");
	expect(config.zed->resolution == "AUTO", "absolute extends path should inherit unresolved base fields");
}


void test_missing_parent_file_fails_with_readable_error() {
	TempDir dir;
	const auto child_path = dir.path() / "missing-parent.toml";
	write_file(
		child_path,
		R"(extends = "does-not-exist.toml"
		name = "broken"
		)"
	);

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(child_path); },
		"extends missing file `does-not-exist.toml`");
}

void test_inheritance_cycle_fails_with_readable_error() {
	TempDir dir;
	const auto a_path = dir.path() / "a.toml";
	const auto b_path = dir.path() / "b.toml";
	write_file(a_path, "extends = \"b.toml\"\nname = \"a\"\n");
	write_file(b_path, "extends = \"a.toml\"\nname = \"b\"\n");

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(a_path); },
		"config inheritance cycle detected:");
}

void test_bad_extends_type_fails() {
	TempDir dir;
	const auto child_path = dir.path() / "bad-extends.toml";
	write_file(
		child_path,
		R"(extends = 42
		name = "broken"
		)"
	);

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(child_path); },
		"has non-string `extends`");
}

std::string udp_rtp_config_toml(std::string_view udp_rtp_body) {
	return
		"name = \"udp\"\n"
		"\n"
		"[ipc]\n"
		"namespace = \"cvmmap\"\n"
		"prefix = \"/tmp\"\n"
		"\n"
		"[video]\n"
		"backend = \"udp_rtp\"\n"
		"finite_stream_ending_behavior = \"stop\"\n"
		"\n"
		"[udp_rtp]\n" + std::string(udp_rtp_body);
}

void test_udp_rtp_defaults_to_h265_codec() {
	TempDir dir;
	const auto path = dir.path() / "udp-default.toml";
	write_file(
		path,
		udp_rtp_config_toml(
			"multicast_group = \"224.0.0.123\"\n"
			"port = 5602\n"
			"payload_type = 96\n"
			"auto_multicast = true\n"
			"decoder = \"auto\"\n"));

	const auto config = app::Config::from_toml(path);
	expect(config.udp_rtp.has_value(), "udp_rtp config should populate udp_rtp section");
	expect(config.udp_rtp->codec == "h265", "udp_rtp codec should default to h265");
	expect(config.udp_rtp->decoder == "auto", "udp_rtp decoder should parse");
}

void test_udp_rtp_accepts_h264_codec_and_decoder() {
	TempDir dir;
	const auto path = dir.path() / "udp-h264.toml";
	write_file(
		path,
		udp_rtp_config_toml(
			"multicast_group = \"224.0.0.123\"\n"
			"codec = \"h264\"\n"
			"decoder = \"avdec_h264\"\n"));

	const auto config = app::Config::from_toml(path);
	expect(config.udp_rtp.has_value(), "udp_rtp config should populate udp_rtp section");
	expect(config.udp_rtp->codec == "h264", "udp_rtp codec should parse h264");
	expect(config.udp_rtp->decoder == "avdec_h264", "udp_rtp decoder should parse avdec_h264");
}

void test_udp_rtp_rejects_unknown_codec() {
	TempDir dir;
	const auto path = dir.path() / "udp-bad-codec.toml";
	write_file(
		path,
		udp_rtp_config_toml(
			"multicast_group = \"224.0.0.123\"\n"
			"codec = \"vp9\"\n"));

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(path); },
		"udp_rtp.codec must be one of: h264, h265");
}

void test_udp_rtp_rejects_decoder_that_does_not_match_codec() {
	TempDir dir;
	const auto path = dir.path() / "udp-mismatched-decoder.toml";
	write_file(
		path,
		udp_rtp_config_toml(
			"multicast_group = \"224.0.0.123\"\n"
			"codec = \"h264\"\n"
			"decoder = \"nvh265dec\"\n"));

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(path); },
		"udp_rtp.decoder must be one of: auto, nvh264dec, avdec_h264 when udp_rtp.codec=h264");
}

void test_mcap_playlist_parses_and_round_trips() {
	TempDir dir;
	const auto path = dir.path() / "mcap-playlist.toml";
	write_file(
		path,
		R"(name = "mcap-playlist"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[video]
backend = "mcap"

[mcap]
video_topic = "/camera/video"
depth_topic = "/camera/depth"
body_topic = "/camera/body"
timestamp_domain = "unix_epoch_ns"

[mcap.playlist]
paths = ["/data/one.mcap", "/data/two.mcap"]
sort_by_recording_time = true
)");

	const auto config = app::Config::from_toml(path);
	expect(config.mcap.has_value(), "mcap playlist config should populate mcap section");
	expect(config.mcap->playlist.has_value(), "mcap playlist config should populate playlist");
	expect(config.mcap->playlist->paths.size() == 2, "mcap playlist should parse both paths");
	expect(config.mcap->playlist->sort_by_recording_time, "mcap playlist should parse sort flag");
	expect(config.mcap->path.empty(), "mcap playlist config should not require mcap.path");

	const auto rendered = config.to_toml();
	expect(rendered.find("[mcap.playlist]") != std::string::npos, "mcap playlist should round-trip");
	expect(rendered.find("sort_by_recording_time = true") != std::string::npos, "mcap playlist sort flag should round-trip");
}

void test_mcap_rejects_path_and_playlist_together() {
	TempDir dir;
	const auto path = dir.path() / "mcap-both.toml";
	write_file(
		path,
		R"(name = "mcap-both"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[video]
backend = "mcap"

[mcap]
path = "/data/one.mcap"

[mcap.playlist]
paths = ["/data/two.mcap"]
)");

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(path); },
		"exactly one of mcap.path or mcap.playlist.paths must be configured");
}

void test_mcap_playlist_rejects_non_boolean_sort_flag() {
	TempDir dir;
	const auto path = dir.path() / "mcap-invalid-sort.toml";
	write_file(
		path,
		R"(name = "mcap-invalid-sort"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[video]
backend = "mcap"

[mcap]
video_topic = "/camera/video"

[mcap.playlist]
paths = ["/data/one.mcap"]
sort_by_recording_time = "yes"
)");

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(path); },
		"mcap.playlist.sort_by_recording_time must be boolean");
}

void test_zed_playlist_requires_svo_mode() {
	TempDir dir;
	const auto path = dir.path() / "zed-playlist-local.toml";
	write_file(
		path,
		R"(name = "zed-playlist-local"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[video]
backend = "zed"

[zed]
stream_mode = "local"
index = 0
resolution = "AUTO"
fps = 30
depth_mode = "NONE"

[zed.playlist]
paths = ["/data/example.svo2"]
)");

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(path); },
		"zed.playlist is only valid when zed.stream_mode is svo");
}

void test_zed_playlist_parses_without_svo_path() {
	TempDir dir;
	const auto path = dir.path() / "zed-playlist.toml";
	write_file(
		path,
		R"(name = "zed-playlist"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[video]
backend = "zed"

[zed]
stream_mode = "svo"
resolution = "AUTO"
fps = 30
depth_mode = "NEURAL"

[zed.playlist]
paths = ["/data/example1.svo2", "/data/example2.svo2"]
sort_by_recording_time = true
)");

	const auto config = app::Config::from_toml(path);
	expect(config.zed.has_value(), "zed playlist config should populate zed section");
	expect(config.zed->playlist.has_value(), "zed playlist config should populate playlist");
	expect(config.zed->playlist->paths.size() == 2, "zed playlist should parse both paths");
	expect(config.zed->playlist->sort_by_recording_time, "zed playlist should parse sort flag");
	expect(!config.zed->svo_path.has_value(), "zed playlist config should not require zed.svo_path");
	expect(config.zed->publish_confidence, "zed playlist config should default publish_confidence to true");
	expect(config.zed->svo_real_time_mode, "zed playlist config should default svo_real_time_mode to true");
	expect(config.zed->depth_stabilization == 30, "zed playlist config should default depth_stabilization to 30");
}

void test_zed_runtime_knobs_parse_and_round_trip() {
	TempDir dir;
	const auto path = dir.path() / "zed-runtime-knobs.toml";
	write_file(
		path,
		R"(name = "zed-runtime-knobs"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[video]
backend = "zed"

[zed]
stream_mode = "svo"
svo_path = "/data/example.svo2"
resolution = "AUTO"
fps = 30
depth_mode = "NEURAL"
publish_confidence = false
svo_real_time_mode = false
depth_stabilization = 0
)");

	const auto config = app::Config::from_toml(path);
	expect(config.zed.has_value(), "zed runtime knob config should populate zed section");
	expect(!config.zed->publish_confidence, "zed runtime knob config should parse publish_confidence");
	expect(!config.zed->svo_real_time_mode, "zed runtime knob config should parse svo_real_time_mode");
	expect(config.zed->depth_stabilization == 0, "zed runtime knob config should parse depth_stabilization");

	const auto rendered = config.to_toml();
	expect(rendered.find("publish_confidence = false") != std::string::npos, "zed publish_confidence should round-trip");
	expect(rendered.find("svo_real_time_mode = false") != std::string::npos, "zed svo_real_time_mode should round-trip");
	expect(rendered.find("depth_stabilization = 0") != std::string::npos, "zed depth_stabilization should round-trip");
}

void test_zed_rejects_out_of_range_depth_stabilization() {
	TempDir dir;
	const auto path = dir.path() / "zed-bad-depth-stabilization.toml";
	write_file(
		path,
		R"(name = "zed-bad-depth-stabilization"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[video]
backend = "zed"

[zed]
stream_mode = "local"
index = 0
resolution = "AUTO"
fps = 30
depth_mode = "NONE"
depth_stabilization = 101
)");

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(path); },
		"zed.depth_stabilization must be in [0, 100]");
}

void test_zed_rejects_svo_path_and_playlist_together() {
	TempDir dir;
	const auto path = dir.path() / "zed-both.toml";
	write_file(
		path,
		R"(name = "zed-both"

[ipc]
namespace = "cvmmap"
prefix = "/tmp"

[video]
backend = "zed"

[zed]
stream_mode = "svo"
svo_path = "/data/example.svo2"
resolution = "AUTO"
fps = 30
depth_mode = "NEURAL"

[zed.playlist]
paths = ["/data/example2.svo2"]
)");

	expect_throws_contains(
		[&] { (void)app::Config::from_toml(path); },
		"exactly one of zed.svo_path or zed.playlist.paths must be configured when zed.stream_mode is svo");
}

bool run_test(const std::string_view name, const std::function<void()> &fn) {
	try {
		fn();
		return true;
	} catch (const std::exception &e) {
		std::cerr << name << " failed: " << e.what() << '\n';
		return false;
	}
}

} // namespace

int main() {
	bool ok = true;
	ok &= run_test("loads_base_file_directly", test_loads_base_file_directly);
	ok &= run_test("loads_derived_file_and_inherits_unspecified_fields", test_loads_derived_file_and_inherits_unspecified_fields);
	ok &= run_test("derived_file_overrides_name", test_derived_file_overrides_name);
	ok &= run_test("derived_file_overrides_zed_index", test_derived_file_overrides_zed_index);
	ok &= run_test("nested_table_override_leaves_unrelated_base_keys_intact", test_nested_table_override_leaves_unrelated_base_keys_intact);
	ok &= run_test("array_override_replaces_parent_array", test_array_override_replaces_parent_array);
	ok &= run_test("absolute_parent_path_is_supported", test_absolute_parent_path_is_supported);
	ok &= run_test("missing_parent_file_fails_with_readable_error", test_missing_parent_file_fails_with_readable_error);
	ok &= run_test("inheritance_cycle_fails_with_readable_error", test_inheritance_cycle_fails_with_readable_error);
	ok &= run_test("bad_extends_type_fails", test_bad_extends_type_fails);
	ok &= run_test("udp_rtp_defaults_to_h265_codec", test_udp_rtp_defaults_to_h265_codec);
	ok &= run_test("udp_rtp_accepts_h264_codec_and_decoder", test_udp_rtp_accepts_h264_codec_and_decoder);
	ok &= run_test("udp_rtp_rejects_unknown_codec", test_udp_rtp_rejects_unknown_codec);
	ok &= run_test("udp_rtp_rejects_decoder_that_does_not_match_codec", test_udp_rtp_rejects_decoder_that_does_not_match_codec);
	ok &= run_test("mcap_playlist_parses_and_round_trips", test_mcap_playlist_parses_and_round_trips);
	ok &= run_test("mcap_rejects_path_and_playlist_together", test_mcap_rejects_path_and_playlist_together);
	ok &= run_test("mcap_playlist_rejects_non_boolean_sort_flag", test_mcap_playlist_rejects_non_boolean_sort_flag);
	ok &= run_test("zed_playlist_requires_svo_mode", test_zed_playlist_requires_svo_mode);
	ok &= run_test("zed_playlist_parses_without_svo_path", test_zed_playlist_parses_without_svo_path);
	ok &= run_test("zed_runtime_knobs_parse_and_round_trip", test_zed_runtime_knobs_parse_and_round_trip);
	ok &= run_test("zed_rejects_out_of_range_depth_stabilization", test_zed_rejects_out_of_range_depth_stabilization);
	ok &= run_test("zed_rejects_svo_path_and_playlist_together", test_zed_rejects_svo_path_and_playlist_together);
	return ok ? 0 : 1;
}
