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
use_finite_as_infinite_stream = false
finite_stream_ending_behavior = "loop"

[zed]
stream_mode = "local"
index = 0
resolution = "AUTO"
fps = 30
depth_mode = "NONE"
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
	return ok ? 0 : 1;
}
