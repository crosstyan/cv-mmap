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

####### BACKENDS #######
add_subdirectory(backends)
add_library(app_video_backends STATIC)
target_include_directories(app_video_backends PUBLIC backends)

option(WITH_OPENCV_BACKEND "Build with OpenCV support" ON)
if (WITH_OPENCV_BACKEND)
        find_package(OpenCV REQUIRED)
        target_compile_definitions(app_video_backends PRIVATE WITH_OPENCV_BACKEND=1)
        target_sources(app_video_backends PRIVATE backends/app_backends_opencv.cpp)
        target_include_directories(app_video_backends PRIVATE ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(app_video_backends PRIVATE ${OpenCV_LIBS})
endif()

option(WITH_GSTREAMER_BACKEND "Build with GStreamer support" ON)
if (WITH_GSTREAMER_BACKEND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GST REQUIRED 
        gstreamer-1.0>=1.14 
        gstreamer-video-1.0>=1.14
        gstreamer-app-1.0>=1.14)
        target_compile_definitions(app_video_backends PRIVATE WITH_GSTREAMER_BACKEND=1) 
        target_sources(app_video_backends PRIVATE backends/app_backends_gstreamer.cpp)
        target_include_directories(app_video_backends PRIVATE ${GST_INCLUDE_DIRS})
        target_link_libraries(app_video_backends PRIVATE ${GST_LIBRARIES})
endif()
target_link_libraries(app_video_backends PUBLIC msft_proxy4::proxy)

add_library(app INTERFACE)
target_include_directories(app INTERFACE .)
target_link_libraries(app INTERFACE app_video_backends app_version app_models app_config)
