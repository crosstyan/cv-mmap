cmake_minimum_required(VERSION 4.0)
# https://github.com/Kitware/CMake/blob/v4.0.3/Help/dev/experimental.rst
# In order to activate support for import std in C++23 and newer targets, set
# variable CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 
#
# This specific value changes as experimental support evolves. See
# `Help/dev/experimental.rst` in the CMake source corresponding to
# your CMake build for the exact value to use.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(cv-mmap VERSION 0.1.0 LANGUAGES CXX)
find_package(OpenCV REQUIRED)
# currently `clangd` only work with `clang++`
# so toolchain kit need to be set to `clang`

# brew install cppzmq
# or
# apt-get install libzmq3-dev
find_package(cppzmq REQUIRED)

add_subdirectory(lib/tomlplusplus)
add_subdirectory(lib/CLI11)

option(BUILD_SPDLOG "Build spdlog" OFF)
if (BUILD_SPDLOG)
    set(SPDLOG_USE_STD_FORMAT ON)
    add_subdirectory(lib/spdlog)
endif ()

# on structured logging
# https://news.ycombinator.com/item?id=28760166
if (NOT TARGET spdlog)
    find_package(spdlog REQUIRED)
endif ()

add_executable(cv-mmap)
target_sources(cv-mmap PUBLIC src/main.cpp)
# https://cmake.org/cmake/help/latest/command/target_sources.html
target_sources(cv-mmap 
        PUBLIC 
                FILE_SET app_module 
                TYPE CXX_MODULES 
                FILES 
                        src/app/app.cxx
                        src/app/app-types.cxx
                        src/app/app-utils.cxx)
target_include_directories(cv-mmap PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(cv-mmap ${OpenCV_LIBS} cppzmq)
target_link_libraries(cv-mmap CLI11::CLI11 tomlplusplus::tomlplusplus spdlog)
target_link_options(cv-mmap PRIVATE "LINKER:--copy-dt-needed-entries")
set_target_properties(cv-mmap PROPERTIES CXX_MODULE_STD ON)


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
