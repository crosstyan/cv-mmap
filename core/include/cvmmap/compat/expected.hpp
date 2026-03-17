#pragma once

#if defined(CVMMAP_HAS_STD_EXPECTED)
#define CVMMAP_COMPAT_USE_STD_EXPECTED CVMMAP_HAS_STD_EXPECTED
#elif defined(__has_include)
#if __has_include(<expected>)
#define CVMMAP_COMPAT_USE_STD_EXPECTED 1
#else
#define CVMMAP_COMPAT_USE_STD_EXPECTED 0
#endif
#else
#define CVMMAP_COMPAT_USE_STD_EXPECTED 0
#endif

#if CVMMAP_COMPAT_USE_STD_EXPECTED
#include <expected>
#else
#include <tl/expected.hpp>
#endif

#include <utility>

namespace cvmmap {

#if CVMMAP_COMPAT_USE_STD_EXPECTED

template <typename T, typename E>
using expected = std::expected<T, E>;

using std::unexpected;

#else

template <typename T, typename E>
using expected = tl::expected<T, E>;

template <typename E>
[[nodiscard]] constexpr auto unexpected(E &&error) {
	return tl::make_unexpected(std::forward<E>(error));
}

#endif

} // namespace cvmmap

#undef CVMMAP_COMPAT_USE_STD_EXPECTED
