#pragma once

#include <utility>

#if defined(CVMMAP_HAS_STD_FORMAT) && CVMMAP_HAS_STD_FORMAT
#include <format>
#else
#include <fmt/format.h>
#endif

namespace cvmmap {

#if defined(CVMMAP_HAS_STD_FORMAT) && CVMMAP_HAS_STD_FORMAT
using std::format;
#else
template <typename... Args>
[[nodiscard]] inline auto format(fmt::format_string<Args...> fmt_string,
		Args &&...args) {
	return fmt::format(fmt_string, std::forward<Args>(args)...);
}
#endif

} // namespace cvmmap
