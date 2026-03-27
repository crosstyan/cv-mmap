####### MODELS #######
add_library(app_models STATIC models/app_models.cpp)
target_include_directories(app_models PUBLIC models)
target_link_libraries(app_models PUBLIC cvmmap_target)

####### CONFIG #######
add_library(app_config STATIC config/app_config.cpp)
target_include_directories(app_config PUBLIC config)
target_link_libraries(app_config PUBLIC app_models tomlplusplus::tomlplusplus cvmmap_target)

####### VERSION #######
add_library(app_version STATIC version/app_version.cpp)
target_include_directories(app_version PUBLIC version)

# https://www.mattkeeter.com/blog/2018-01-06-versioning/
# version base on commit
execute_process(COMMAND git log --pretty=format:'%h' -n 1
        OUTPUT_VARIABLE GIT_REV
        ERROR_QUIET)

# Check whether we got any revision (which isn't
# always the case, e.g. when someone downloaded a zip
# file from Github instead of a checkout)
if ("${GIT_REV}" STREQUAL "")
    set(GIT_REV "N/A")
    set(GIT_DIFF "")
    set(GIT_TAG "N/A")
    set(GIT_BRANCH "N/A")
else ()
    execute_process(
            COMMAND bash -c "git diff --quiet --exit-code || echo +"
            OUTPUT_VARIABLE GIT_DIFF)
    execute_process(
            COMMAND git describe --exact-match --tags
            OUTPUT_VARIABLE GIT_TAG ERROR_QUIET)
    execute_process(
            COMMAND git rev-parse --abbrev-ref HEAD
            OUTPUT_VARIABLE GIT_BRANCH)

    string(STRIP "${GIT_REV}" GIT_REV)
    string(SUBSTRING "${GIT_REV}" 1 7 GIT_REV)
    string(STRIP "${GIT_DIFF}" GIT_DIFF)
    string(STRIP "${GIT_TAG}" GIT_TAG)
    string(STRIP "${GIT_BRANCH}" GIT_BRANCH)
endif ()
string(TIMESTAMP COMPILE_TIMESTAMP_LOCAL "%Y-%m-%dT%H:%M:%S%z")
string(TIMESTAMP COMPILE_TIMESTAMP_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
target_compile_definitions(app_version
        PRIVATE
        GIT_REV=${GIT_REV}
        GIT_TAG=${GIT_TAG}
        GIT_BRANCH=${GIT_BRANCH}
        GIT_DIFF=${GIT_DIFF}
        COMPILE_TIMESTAMP_LOCAL=${COMPILE_TIMESTAMP_LOCAL}
        COMPILE_TIMESTAMP_UTC=${COMPILE_TIMESTAMP_UTC}
)

####### BACKEND OPTIONS #######
function(cvmmap_resolve_backend_mode out_var cache_var legacy_var default_value description)
	if (DEFINED ${legacy_var} AND NOT DEFINED ${cache_var})
		if (${legacy_var})
			set(_legacy_value ON)
		else()
			set(_legacy_value OFF)
		endif()
		set(${cache_var} "${_legacy_value}" CACHE STRING "${description}" FORCE)
		message(WARNING "${legacy_var} is deprecated; use ${cache_var}=ON|OFF|AUTO instead")
	elseif (NOT DEFINED ${cache_var})
		set(${cache_var} "${default_value}" CACHE STRING "${description}")
	endif()

	set_property(CACHE ${cache_var} PROPERTY STRINGS AUTO ON OFF)
	string(TOUPPER "${${cache_var}}" _mode)
	if (NOT _mode STREQUAL "AUTO" AND NOT _mode STREQUAL "ON" AND NOT _mode STREQUAL "OFF")
		message(FATAL_ERROR "${cache_var} must be one of AUTO, ON, or OFF (got '${${cache_var}}')")
	endif()
	set(${out_var} "${_mode}" PARENT_SCOPE)
endfunction()

cvmmap_resolve_backend_mode(BUILD_BACKEND_OPENCV_MODE BUILD_BACKEND_OPENCV WITH_BACKEND_OPENCV AUTO
	"Build OpenCV backend support (AUTO, ON, OFF)")
cvmmap_resolve_backend_mode(BUILD_BACKEND_GSTREAMER_MODE BUILD_BACKEND_GSTREAMER WITH_BACKEND_GSTREAMER AUTO
	"Build GStreamer backend support (AUTO, ON, OFF)")
cvmmap_resolve_backend_mode(BUILD_BACKEND_MCAP_MODE BUILD_BACKEND_MCAP WITH_BACKEND_MCAP AUTO
	"Build MCAP backend support (AUTO, ON, OFF)")
cvmmap_resolve_backend_mode(BUILD_BACKEND_ZED_MODE BUILD_BACKEND_ZED WITH_BACKEND_ZED OFF
	"Build ZED backend support (AUTO, ON, OFF)")

set(WITH_BACKEND_OPENCV OFF)
if (BUILD_BACKEND_OPENCV_MODE STREQUAL "ON")
	find_package(OpenCV REQUIRED COMPONENTS videoio)
	set(WITH_BACKEND_OPENCV ON)
	message(STATUS "OpenCV backend: enabled (BUILD_BACKEND_OPENCV=ON)")
elseif (BUILD_BACKEND_OPENCV_MODE STREQUAL "AUTO")
	find_package(OpenCV QUIET COMPONENTS videoio)
	if (OpenCV_FOUND)
		set(WITH_BACKEND_OPENCV ON)
		message(STATUS "OpenCV backend: enabled (auto-detected)")
	else()
		message(STATUS "OpenCV backend: disabled (OpenCV not found)")
	endif()
else()
	message(STATUS "OpenCV backend: disabled (BUILD_BACKEND_OPENCV=OFF)")
endif()

set(WITH_BACKEND_GSTREAMER OFF)
set(_GSTREAMER_DEPS_FOUND OFF)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
	pkg_check_modules(GST QUIET IMPORTED_TARGET
		gstreamer-1.0>=1.14
		gstreamer-video-1.0>=1.14
		gstreamer-app-1.0>=1.14)
	if (GST_FOUND)
		set(_GSTREAMER_DEPS_FOUND ON)
	endif()
endif()

if (BUILD_BACKEND_GSTREAMER_MODE STREQUAL "ON")
	if (NOT PkgConfig_FOUND)
		message(FATAL_ERROR "BUILD_BACKEND_GSTREAMER=ON requires PkgConfig")
	endif()
	if (NOT _GSTREAMER_DEPS_FOUND)
		message(FATAL_ERROR "BUILD_BACKEND_GSTREAMER=ON requires gstreamer-1.0, gstreamer-video-1.0, and gstreamer-app-1.0 (>= 1.14)")
	endif()
	set(WITH_BACKEND_GSTREAMER ON)
	message(STATUS "GStreamer backend: enabled (BUILD_BACKEND_GSTREAMER=ON)")
elseif (BUILD_BACKEND_GSTREAMER_MODE STREQUAL "AUTO")
	if (_GSTREAMER_DEPS_FOUND)
		set(WITH_BACKEND_GSTREAMER ON)
		message(STATUS "GStreamer backend: enabled (auto-detected)")
	else()
		message(STATUS "GStreamer backend: disabled (GStreamer not found)")
	endif()
else()
	message(STATUS "GStreamer backend: disabled (BUILD_BACKEND_GSTREAMER=OFF)")
endif()

set(WITH_BACKEND_MCAP OFF)
set(_MCAP_DEPS_FOUND OFF)
set(_MCAP_INCLUDE_DIR "")
set(CVMMAP_MCAP_INCLUDE_DIR "" CACHE PATH
	"Optional path to MCAP C++ headers (the directory containing mcap/reader.hpp)")
if (CVMMAP_MCAP_INCLUDE_DIR)
	if (EXISTS "${CVMMAP_MCAP_INCLUDE_DIR}/mcap/reader.hpp")
		set(_MCAP_INCLUDE_DIR "${CVMMAP_MCAP_INCLUDE_DIR}")
	else()
		message(FATAL_ERROR
			"CVMMAP_MCAP_INCLUDE_DIR='${CVMMAP_MCAP_INCLUDE_DIR}' does not contain mcap/reader.hpp")
	endif()
elseif (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mcap/include/mcap/reader.hpp")
	set(_MCAP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mcap/include")
endif()

find_package(Protobuf QUIET)
set(_MCAP_PROTOBUF_FOUND OFF)
if (Protobuf_FOUND)
	set(_MCAP_PROTOBUF_FOUND ON)
endif()

set(_MCAP_FFMPEG_FOUND OFF)
if (PkgConfig_FOUND)
	pkg_check_modules(FFMPEG_LIBAV QUIET IMPORTED_TARGET libavcodec libavutil libswscale)
	if (FFMPEG_LIBAV_FOUND)
		set(_MCAP_FFMPEG_FOUND ON)
	endif()
	pkg_check_modules(MCAP_COMPRESSION_LIBS QUIET IMPORTED_TARGET libzstd liblz4)
endif()

find_package(rvl CONFIG QUIET)
if (NOT TARGET rvl::rvl)
	set(RVL_LOCAL_ROOT "/home/crosstyan/Code/rvl_impl")
	set(RVL_LOCAL_BUILD "${RVL_LOCAL_ROOT}/build/core")
	if (
		EXISTS "${RVL_LOCAL_ROOT}/core/include/rvl/rvl.hpp"
		AND EXISTS "${RVL_LOCAL_BUILD}/librvl_core.a")
		add_library(rvl::rvl INTERFACE IMPORTED)
		set_target_properties(rvl::rvl PROPERTIES
			INTERFACE_INCLUDE_DIRECTORIES "${RVL_LOCAL_ROOT}/core/include"
			INTERFACE_LINK_LIBRARIES "${RVL_LOCAL_BUILD}/librvl_core.a")
	endif()
endif()

if (_MCAP_INCLUDE_DIR AND _MCAP_PROTOBUF_FOUND AND _MCAP_FFMPEG_FOUND AND TARGET rvl::rvl)
	set(_MCAP_DEPS_FOUND ON)
endif()

if (BUILD_BACKEND_MCAP_MODE STREQUAL "ON")
	if (NOT _MCAP_DEPS_FOUND)
		message(FATAL_ERROR
			"BUILD_BACKEND_MCAP=ON requires mcap headers, Protobuf, libavcodec/libavutil/libswscale, and rvl::rvl. "
			"Set CVMMAP_MCAP_INCLUDE_DIR if the headers are not vendored under third_party/mcap/include.")
	endif()
	set(WITH_BACKEND_MCAP ON)
	message(STATUS "MCAP backend: enabled (BUILD_BACKEND_MCAP=ON)")
elseif (BUILD_BACKEND_MCAP_MODE STREQUAL "AUTO")
	if (_MCAP_DEPS_FOUND)
		set(WITH_BACKEND_MCAP ON)
		message(STATUS "MCAP backend: enabled (auto-detected)")
	else()
		message(STATUS "MCAP backend: disabled (dependencies not found)")
	endif()
else()
	message(STATUS "MCAP backend: disabled (BUILD_BACKEND_MCAP=OFF)")
endif()

set(WITH_BACKEND_ZED OFF)
if (BUILD_BACKEND_ZED_MODE STREQUAL "AUTO")
	message(FATAL_ERROR "BUILD_BACKEND_ZED=AUTO is not supported; use ON or OFF")
elseif (BUILD_BACKEND_ZED_MODE STREQUAL "ON")
	set(WITH_BACKEND_ZED ON)
	message(STATUS "ZED backend: enabled (BUILD_BACKEND_ZED=ON)")
else()
	message(STATUS "ZED backend: disabled (BUILD_BACKEND_ZED=OFF)")
endif()

####### PREPROCESS #######
add_library(app_preprocess STATIC preprocess/app_preprocess_undistort.cpp)
target_include_directories(app_preprocess PUBLIC preprocess)
target_link_libraries(app_preprocess PUBLIC app_config app_models)

set(_WITH_PREPROCESS_UNDISTORT_OPENCV_DEFAULT OFF)
if (DEFINED WITH_PREPROCESS_UNDISTORT_OPENCV)
	set(_WITH_PREPROCESS_UNDISTORT_OPENCV_DEFAULT ${WITH_PREPROCESS_UNDISTORT_OPENCV})
else()
	set(_WITH_PREPROCESS_UNDISTORT_OPENCV_DEFAULT ${WITH_BACKEND_OPENCV})
endif()
option(WITH_PREPROCESS_UNDISTORT_OPENCV "Build OpenCV-accelerated undistort preprocess support" ${_WITH_PREPROCESS_UNDISTORT_OPENCV_DEFAULT})
if (WITH_PREPROCESS_UNDISTORT_OPENCV)
	find_package(OpenCV REQUIRED COMPONENTS calib3d imgproc core)
	message(STATUS "undistort preprocess: enabling OpenCV remap path")
	target_compile_definitions(app_preprocess PUBLIC WITH_PREPROCESS_UNDISTORT_OPENCV=1)
	target_include_directories(app_preprocess PRIVATE ${OpenCV_INCLUDE_DIRS})
	target_link_libraries(app_preprocess PRIVATE opencv_calib3d opencv_imgproc opencv_core)
else()
	message(WARNING "undistort preprocess: building internal fallback algorithm; repeated-frame performance will be significantly slower than the OpenCV remap path")
	target_compile_definitions(app_preprocess PRIVATE WITH_PREPROCESS_UNDISTORT_INTERNAL_FALLBACK=1)
endif()

####### BACKENDS #######
if (WITH_BACKEND_MCAP)
	add_library(app_mcap_headers INTERFACE)
	target_include_directories(app_mcap_headers INTERFACE "${_MCAP_INCLUDE_DIR}")

	add_library(app_mcap_proto STATIC)
	target_include_directories(app_mcap_proto
		PUBLIC
			"${CMAKE_CURRENT_BINARY_DIR}"
			"${CMAKE_CURRENT_SOURCE_DIR}/proto")
	protobuf_generate(
		TARGET app_mcap_proto
		LANGUAGE cpp
		PROTOS
			"${CMAKE_CURRENT_SOURCE_DIR}/proto/foxglove/CompressedVideo.proto"
			"${CMAKE_CURRENT_SOURCE_DIR}/proto/cvmmap_streamer/DepthMap.proto")
	if (TARGET protobuf::libprotobuf)
		target_link_libraries(app_mcap_proto PUBLIC protobuf::libprotobuf)
	elseif (TARGET Protobuf::libprotobuf)
		target_link_libraries(app_mcap_proto PUBLIC Protobuf::libprotobuf)
	else()
		target_link_libraries(app_mcap_proto PUBLIC ${Protobuf_LIBRARIES})
	endif()
endif()

add_library(app_video_backends STATIC)
target_include_directories(app_video_backends PUBLIC backends)
target_sources(app_video_backends PRIVATE backends/app_backends_dummy.cpp)
target_link_libraries(app_video_backends PRIVATE cvmmap_stb_truetype_headers)
target_compile_definitions(app_video_backends
	PRIVATE
		CVMMAP_DUMMY_DEFAULT_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../third_party/fonts/JetBrainsMono-Regular.ttf")

if (WITH_BACKEND_OPENCV)
		target_compile_definitions(app_video_backends PUBLIC WITH_BACKEND_OPENCV=1)
		target_sources(app_video_backends PRIVATE backends/app_backends_opencv.cpp)
		target_include_directories(app_video_backends PRIVATE ${OpenCV_INCLUDE_DIRS})
		target_link_libraries(app_video_backends PRIVATE opencv_videoio)
endif()
if (WITH_BACKEND_GSTREAMER)
        target_compile_definitions(app_video_backends PUBLIC WITH_BACKEND_GSTREAMER=1) 
        target_sources(app_video_backends PRIVATE backends/app_backends_gst.cpp)
        target_sources(app_video_backends PRIVATE backends/app_backends_udp_rtp.cpp)
        target_link_libraries(app_video_backends PRIVATE PkgConfig::GST)
endif()
if (WITH_BACKEND_MCAP)
		target_compile_definitions(app_video_backends PUBLIC WITH_BACKEND_MCAP=1)
		target_sources(app_video_backends PRIVATE backends/app_backends_mcap.cpp)
		target_link_libraries(app_video_backends PRIVATE app_mcap_headers app_mcap_proto PkgConfig::FFMPEG_LIBAV rvl::rvl)
		if (TARGET PkgConfig::MCAP_COMPRESSION_LIBS)
			target_link_libraries(app_video_backends PRIVATE PkgConfig::MCAP_COMPRESSION_LIBS)
		endif()
endif()

if (WITH_BACKEND_ZED)
        option(LINK_SHARED_ZED "Link with the ZED SDK shared executable" ON)
        if (NOT LINK_SHARED_ZED AND MSVC)
                message(FATAL_ERROR "LINK_SHARED_ZED OFF : ZED SDK static libraries not available on Windows")
        endif()

        find_package(ZED REQUIRED)
        find_package(CUDA ${ZED_CUDA_VERSION} REQUIRED)

        target_include_directories(app_video_backends PRIVATE ${CUDA_INCLUDE_DIRS} ${ZED_INCLUDE_DIRS})
        target_link_directories(app_video_backends PRIVATE ${ZED_LIBRARY_DIR} ${CUDA_LIBRARY_DIRS})

        if (LINK_SHARED_ZED)
                set(ZED_LIBS ${ZED_LIBRARIES} ${CUDA_CUDA_LIBRARY} ${CUDA_CUDART_LIBRARY})
        else()
                set(ZED_LIBS ${ZED_STATIC_LIBRARIES} ${CUDA_CUDA_LIBRARY} ${CUDA_LIBRARY})
        endif()

        target_compile_definitions(app_video_backends PUBLIC WITH_BACKEND_ZED=1)
        target_sources(app_video_backends PRIVATE backends/app_backends_zed.cpp)
        target_link_libraries(app_video_backends PRIVATE ${ZED_LIBS})
endif()

target_link_libraries(app_video_backends PUBLIC app_models)
target_link_libraries(app_video_backends PUBLIC app_config)
target_link_libraries(app_video_backends PUBLIC cvmmap_ipc)

add_library(app INTERFACE)
target_include_directories(app INTERFACE .)
target_link_libraries(app INTERFACE app_video_backends app_version app_models app_config app_preprocess)
target_compile_definitions(app INTERFACE $<$<CONFIG:Debug>:APP_DEBUG>)

option(BUILD_PREPROCESS_BENCHMARKS "Build preprocess benchmark executables" OFF)
if (BUILD_PREPROCESS_BENCHMARKS)
	add_executable(app_preprocess_benchmark preprocess/benchmark_undistort.cpp)
	target_include_directories(app_preprocess_benchmark PRIVATE .)
target_link_libraries(app_preprocess_benchmark PRIVATE app_preprocess spdlog::spdlog fmt::fmt)
	if (WITH_PREPROCESS_UNDISTORT_OPENCV)
		target_compile_definitions(app_preprocess_benchmark PRIVATE WITH_PREPROCESS_UNDISTORT_OPENCV=1)
	endif()
endif()


if (CVMMAP_BUILD_TESTS)
	add_executable(cvmmap_app_config_tests
		tests/app_config_tests.cpp
)
	target_include_directories(cvmmap_app_config_tests PRIVATE config)
	target_link_libraries(cvmmap_app_config_tests
		PRIVATE
			app_config
			app_models
			cvmmap_target
			spdlog::spdlog
)
	add_test(
		NAME cvmmap_app_config_tests
		COMMAND cvmmap_app_config_tests
	)
	add_subdirectory(tests)
endif()
