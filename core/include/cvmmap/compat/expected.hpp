#pragma once

#if defined(CVMMAP_HAS_STD_EXPECTED) && CVMMAP_HAS_STD_EXPECTED
#include <expected>
#else
#include <tl/expected.hpp>
#endif

#include <utility>

namespace cvmmap {

#if defined(CVMMAP_HAS_STD_EXPECTED) && CVMMAP_HAS_STD_EXPECTED

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
