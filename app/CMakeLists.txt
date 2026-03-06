set(APP_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib)

####### SPDLOG #######
# TODO: consider using structured logging
# https://news.ycombinator.com/item?id=28760166
option(BUILD_SPDLOG "Build spdlog" OFF)
if (BUILD_SPDLOG)
    set(SPDLOG_USE_STD_FORMAT ON)
    add_subdirectory(lib/spdlog)
endif ()
if (NOT TARGET spdlog)
    find_package(spdlog REQUIRED)
endif ()

####### MODELS #######
add_library(app_models STATIC models/app_models.cpp)
target_include_directories(app_models PUBLIC models)

####### CONFIG #######
add_library(app_config STATIC config/app_config.cpp)
target_include_directories(app_config PUBLIC config)
add_subdirectory(${APP_LIB_DIR}/tomlplusplus)
target_link_libraries(app_config PUBLIC app_models tomlplusplus::tomlplusplus)

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

add_subdirectory(lib/proxy)

####### BACKEND OPTIONS #######
option(WITH_BACKEND_OPENCV "Build with OpenCV support" OFF)
option(WITH_BACKEND_GSTREAMER "Build with GStreamer support" ON)
option(WITH_BACKEND_ZED "Build with ZED SDK support" OFF)

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
add_library(app_video_backends STATIC)
target_include_directories(app_video_backends PUBLIC backends)

if (WITH_BACKEND_OPENCV)
		find_package(OpenCV REQUIRED COMPONENTS videoio)
		target_compile_definitions(app_video_backends PUBLIC WITH_BACKEND_OPENCV=1)
		target_sources(app_video_backends PRIVATE backends/app_backends_opencv.cpp)
		target_include_directories(app_video_backends PRIVATE ${OpenCV_INCLUDE_DIRS})
		target_link_libraries(app_video_backends PRIVATE opencv_videoio)
endif()
if (WITH_BACKEND_GSTREAMER)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GST REQUIRED 
        gstreamer-1.0>=1.14 
        gstreamer-video-1.0>=1.14
        gstreamer-app-1.0>=1.14)
        target_compile_definitions(app_video_backends PUBLIC WITH_BACKEND_GSTREAMER=1) 
        target_sources(app_video_backends PRIVATE backends/app_backends_gst.cpp)
        target_include_directories(app_video_backends PRIVATE ${GST_INCLUDE_DIRS})
        target_link_libraries(app_video_backends PRIVATE ${GST_LIBRARIES})
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

if (NOT WITH_BACKEND_OPENCV AND NOT WITH_BACKEND_GSTREAMER AND NOT WITH_BACKEND_ZED)
        message(FATAL_ERROR "At least one backend must be enabled")
endif()
target_link_libraries(app_video_backends PUBLIC msft_proxy4::proxy)
target_link_libraries(app_video_backends PUBLIC app_models)
target_link_libraries(app_video_backends PUBLIC app_config)

add_library(app INTERFACE)
target_include_directories(app INTERFACE .)
target_link_libraries(app INTERFACE app_video_backends app_version app_models app_config app_preprocess)
target_compile_definitions(app INTERFACE $<$<CONFIG:Debug>:APP_DEBUG>)

option(BUILD_PREPROCESS_BENCHMARKS "Build preprocess benchmark executables" OFF)
if (BUILD_PREPROCESS_BENCHMARKS)
	add_executable(app_preprocess_benchmark preprocess/benchmark_undistort.cpp)
	target_include_directories(app_preprocess_benchmark PRIVATE .)
	target_link_libraries(app_preprocess_benchmark PRIVATE app_preprocess spdlog fmt::fmt)
	if (WITH_PREPROCESS_UNDISTORT_OPENCV)
		target_compile_definitions(app_preprocess_benchmark PRIVATE WITH_PREPROCESS_UNDISTORT_OPENCV=1)
	endif()
endif()
