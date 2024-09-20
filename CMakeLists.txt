cmake_minimum_required(VERSION 3.10)
project(cv-mmap)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(OpenCV REQUIRED)
# brew install cppzmq
# or
# apt-get install libzmq3-dev
find_package(cppzmq)

add_subdirectory(lib/tomlplusplus)
add_subdirectory(lib/CLI11)

find_package(fmt REQUIRED)
option(BUILD_SPDLOG "Build spdlog" OFF)
if (BUILD_SPDLOG)
    add_subdirectory(lib/spdlog)
endif ()
# on structured logging
# https://news.ycombinator.com/item?id=28760166
if (NOT TARGET spdlog)
    find_package(spdlog REQUIRED)
endif ()

add_executable(cv-mmap src/main.cpp)
target_include_directories(cv-mmap PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(cv-mmap ${OpenCV_LIBS} cppzmq)
target_link_libraries(cv-mmap CLI11::CLI11 tomlplusplus::tomlplusplus fmt::fmt spdlog::spdlog)


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
string(TIMESTAMP COMPILE_TIMESTAMP "%Y-%m-%dT%H:%M:%S")
target_compile_definitions(cv-mmap
        PRIVATE
        GIT_REV=${GIT_REV}
        GIT_TAG=${GIT_TAG}
        GIT_BRANCH=${GIT_BRANCH}
        COMPILE_TIMESTAMP=${COMPILE_TIMESTAMP}
)
