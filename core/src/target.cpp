#include <cvmmap/target.hpp>
#include <cvmmap/ipc.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <ranges>
#include <stdexcept>
#include <string>

namespace cvmmap {

namespace {

constexpr size_t UNIX_PATH_MAX_LEN = 107;

struct ResolvedTarget {
	std::string instance;
	std::string prefix;
	std::string base_name;
};

bool is_valid_token_char(const char ch) {
	return std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-';
}

bool is_valid_token(const std::string &value, const size_t max_len) {
	if (value.empty() || value.size() > max_len) {
		return false;
	}
	if (!std::isalnum(static_cast<unsigned char>(value.front()))) {
		return false;
	}
	return std::ranges::all_of(value, is_valid_token_char);
}

std::string validate_prefix(std::string prefix) {
	if (prefix.empty() || prefix.front() != '/') {
		throw std::invalid_argument("cvmmap uri prefix must be an absolute path");
	}
	if (prefix.find('\\') != std::string::npos || prefix.find('\0') != std::string::npos) {
		throw std::invalid_argument("cvmmap uri prefix contains invalid characters");
	}
	if (prefix.find("//") != std::string::npos) {
		throw std::invalid_argument("cvmmap uri prefix must not contain empty path segments");
	}
	if (prefix.size() > 1 && prefix.back() == '/') {
		prefix.pop_back();
	}

	size_t start = 1;
	while (start <= prefix.size()) {
		auto end = prefix.find('/', start);
		if (end == std::string::npos) {
			end = prefix.size();
		}
		auto segment = prefix.substr(start, end - start);
		if (segment == "." || segment == "..") {
			throw std::invalid_argument("cvmmap uri prefix must not contain traversal segments");
		}
		if (end == prefix.size()) {
			break;
		}
		start = end + 1;
	}

	return prefix;
}

ResolvedTarget resolve_target(const std::string &name_or_uri) {
	constexpr auto DEFAULT_NAMESPACE = "cvmmap";
	constexpr auto DEFAULT_PREFIX    = "/tmp";

	std::string instance;
	std::string prefix = DEFAULT_PREFIX;
	std::string ns     = DEFAULT_NAMESPACE;

	if (name_or_uri.rfind("cvmmap://", 0) == 0) {
		auto body      = name_or_uri.substr(9);
		auto qpos      = body.find('?');
		auto authority = qpos == std::string::npos ? body : body.substr(0, qpos);
		auto query     = qpos == std::string::npos ? std::string{} : body.substr(qpos + 1);
		if (qpos != std::string::npos && query.empty()) {
			throw std::invalid_argument("cvmmap uri query string is empty");
		}

		auto apos = authority.find('@');
		if (apos != std::string::npos) {
			if (authority.find('@', apos + 1) != std::string::npos) {
				throw std::invalid_argument("cvmmap uri authority must contain at most one '@'");
			}
			instance = authority.substr(0, apos);
			prefix   = authority.substr(apos + 1);
			if (prefix.empty()) {
				throw std::invalid_argument("cvmmap uri prefix is empty");
			}
		} else {
			instance = authority;
		}

		if (!query.empty()) {
			size_t start = 0;
			while (start <= query.size()) {
				auto end = query.find('&', start);
				if (end == std::string::npos) {
					end = query.size();
				}
				auto part = query.substr(start, end - start);
				if (part.empty()) {
					throw std::invalid_argument("cvmmap uri query contains empty key/value");
				}
				auto eq = part.find('=');
				if (eq == std::string::npos) {
					throw std::invalid_argument("cvmmap uri query must be key=value pairs");
				}
				auto key   = part.substr(0, eq);
				auto value = part.substr(eq + 1);
				if (key != "namespace") {
					throw std::invalid_argument(std::format("unsupported cvmmap uri query key: {}", key));
				}
				if (value.empty()) {
					throw std::invalid_argument("cvmmap uri namespace is empty");
				}
				ns = value;
				if (end == query.size()) {
					break;
				}
				start = end + 1;
			}
		}
	} else {
		instance = name_or_uri;
		if (instance.find('@') != std::string::npos || instance.find('?') != std::string::npos ||
			instance.find('/') != std::string::npos) {
			throw std::invalid_argument("plain cvmmap instance names must not contain '@', '?', or '/'");
		}
	}

	if (!is_valid_token(instance, LABEL_LEN_MAX - 1)) {
		throw std::invalid_argument("invalid cvmmap instance; expected [A-Za-z0-9][A-Za-z0-9._-]{0,22}");
	}
	if (!is_valid_token(ns, 32)) {
		throw std::invalid_argument("invalid cvmmap namespace; expected [A-Za-z0-9][A-Za-z0-9._-]{0,31}");
	}

	prefix = validate_prefix(prefix);
	auto base_name = std::format("{}_{}", ns, instance);
	auto control_path = std::format("{}/{}_control", prefix, base_name);
	if (control_path.size() > UNIX_PATH_MAX_LEN) {
		throw std::invalid_argument(std::format("cvmmap ipc path too long ({}>{})", control_path.size(), UNIX_PATH_MAX_LEN));
	}

	return ResolvedTarget{
		.instance  = std::move(instance),
		.prefix    = std::move(prefix),
		.base_name = std::move(base_name),
	};
}

} // namespace

cvmmap_target_t resolve_cvmmap_target_or_throw(const std::string &name_or_uri) {
	const auto resolved = resolve_target(name_or_uri);
	auto target         = cvmmap_target_t{};
	target.instance     = resolved.instance;
	target.prefix       = resolved.prefix;
	target.base_name    = resolved.base_name;
	target.shm_name     = resolved.base_name;
	target.zmq_addr     = std::format("ipc://{}/{}", resolved.prefix, resolved.base_name);
	target.zmq_control_addr = std::format("ipc://{}/{}_control", resolved.prefix, resolved.base_name);
	return target;
}

} // namespace cvmmap
