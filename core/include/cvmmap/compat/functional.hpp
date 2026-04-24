#pragma once

#include <functional>

#ifndef CVMMAP_HAS_STD_MOVE_ONLY_FUNCTION
#if defined(__cpp_lib_move_only_function) && __cpp_lib_move_only_function >= 202110L
#define CVMMAP_HAS_STD_MOVE_ONLY_FUNCTION 1
#else
#define CVMMAP_HAS_STD_MOVE_ONLY_FUNCTION 0
#endif
#endif
#if !defined(CVMMAP_FORCE_COMPAT_MOVE_ONLY_FUNCTION) && CVMMAP_HAS_STD_MOVE_ONLY_FUNCTION

namespace cvmmap {

template <typename Signature>
using move_only_function = std::move_only_function<Signature>;

} // namespace cvmmap

#else

#include <cvmmap/compat/detail/boost_compat/move_only_function.hpp>

namespace cvmmap {

template <typename Signature>
using move_only_function = boost::compat::move_only_function<Signature>;

} // namespace cvmmap

#endif
