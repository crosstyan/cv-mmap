#pragma once

#include <utility>

#if defined(CVMMAP_HAS_STD_FORMAT)
#define CVMMAP_COMPAT_USE_STD_FORMAT CVMMAP_HAS_STD_FORMAT
#elif defined(__has_include)
#if __has_include(<format>)
#define CVMMAP_COMPAT_USE_STD_FORMAT 1
#else
#define CVMMAP_COMPAT_USE_STD_FORMAT 0
#endif
#else
#define CVMMAP_COMPAT_USE_STD_FORMAT 0
#endif

#if CVMMAP_COMPAT_USE_STD_FORMAT
#include <format>
#else
#include <fmt/format.h>
#endif

namespace cvmmap {

#if CVMMAP_COMPAT_USE_STD_FORMAT
using std::format;
#else
template <typename... Args>
[[nodiscard]] inline auto format(fmt::format_string<Args...> fmt_string,
		Args &&...args) {
	return fmt::format(fmt_string, std::forward<Args>(args)...);
}
#endif

} // namespace cvmmap

#undef CVMMAP_COMPAT_USE_STD_FORMAT
