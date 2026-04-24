#include "app_config_internal.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace app::config_detail {

namespace {

std::filesystem::path normalize_config_path(const std::filesystem::path &path) {
	return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

std::string format_inheritance_cycle(
	const std::vector<std::filesystem::path> &chain,
	const std::filesystem::path &repeated_path) {
	std::ostringstream oss;
	bool started = false;
	for (const auto &entry : chain) {
		if (entry == repeated_path) {
			started = true;
		}
		if (!started) {
			continue;
		}
		if (oss.tellp() > 0) {
			oss << " -> ";
		}
		oss << entry.string();
	}
	if (oss.tellp() > 0) {
		oss << " -> ";
	}
	oss << repeated_path.string();
	return oss.str();
}

void merge_toml_tables(toml::table &base, const toml::table &overrides) {
	overrides.for_each([&](const toml::key &key, const auto &overrides_value) {
		const toml::node &overrides_node = overrides_value;
		if (const auto *overrides_table = overrides_node.as_table()) {
			if (auto *base_node = base.get(key)) {
				if (auto *base_table = base_node->as_table()) {
					merge_toml_tables(*base_table, *overrides_table);
					return;
				}
			}
		}

		base.insert_or_assign(key, overrides_node);
	});
}

toml::table load_toml_with_extends(
	const std::filesystem::path &path,
	std::vector<std::filesystem::path> &inheritance_stack) {
	const auto normalized_path = normalize_config_path(path);
	if (std::find(inheritance_stack.begin(), inheritance_stack.end(), normalized_path) != inheritance_stack.end()) {
		throw std::invalid_argument(
			"config inheritance cycle detected: " +
			format_inheritance_cycle(inheritance_stack, normalized_path));
	}

	toml::table current;
	try {
		current = toml::parse_file(normalized_path.string());
	} catch (const toml::parse_error &e) {
		spdlog::error("bad config file parse for `{}`: {}", normalized_path.string(), e.what());
		throw;
	}

	inheritance_stack.push_back(normalized_path);
	if (auto extends_node = current["extends"]; extends_node) {
		auto extends_path = extends_node.value<std::string>();
		if (!extends_path) {
			inheritance_stack.pop_back();
			throw std::invalid_argument(
				"config `" + normalized_path.string() + "` has non-string `extends`");
		}

		const auto referenced_path = std::filesystem::path(*extends_path);
		const auto parent_path = normalize_config_path(
			referenced_path.is_absolute() ? referenced_path : normalized_path.parent_path() / referenced_path);
		if (!std::filesystem::exists(parent_path)) {
			inheritance_stack.pop_back();
			throw std::invalid_argument(
				"config `" + normalized_path.string() + "` extends missing file `" +
				*extends_path + "` (resolved to `" + parent_path.string() + "`)" );
		}

		auto merged = load_toml_with_extends(parent_path, inheritance_stack);
		inheritance_stack.pop_back();
		current.erase("extends");
		merge_toml_tables(merged, current);
		return merged;
	}

	inheritance_stack.pop_back();
	current.erase("extends");
	return current;
}

} // namespace

toml::table load_effective_config_table(const std::filesystem::path &path) {
	std::vector<std::filesystem::path> inheritance_stack;
	return load_toml_with_extends(path, inheritance_stack);
}

} // namespace app::config_detail
