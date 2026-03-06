#pragma once

#include <string>

namespace cvmmap {

struct cvmmap_target_t {
	std::string instance{};
	std::string prefix{};
	std::string base_name{};
	std::string shm_name{};
	std::string zmq_addr{};
	std::string zmq_control_addr{};
};

[[nodiscard]]
cvmmap_target_t resolve_cvmmap_target_or_throw(const std::string &name_or_uri);

} // namespace cvmmap
