#pragma once
#include <string>
#include <variant>
#include <app_models.hpp>
#include <filesystem>

namespace app {
struct Config {
	/// name of shared memory (with `shm_open` and `shm_unlink`)
	std::string name;
	/// pipeline or index, depends on API
	std::variant<std::string, int> pipeline;
	/// API preference used by OpenCV
	VideoCaptureAPIs api_preference = CAP_ANY;
	/// ZMQ address for synchronization
	std::string zmq_address;
	/// whether the video source is looped, when it's a finite source
	bool is_loop = false;

	static Config Default();

	static Config from_toml(const std::filesystem::path &path);

	[[nodiscard]]
	std::string to_toml();
};
}