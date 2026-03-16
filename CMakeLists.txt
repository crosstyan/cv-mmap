cmake_minimum_required(VERSION 3.20)
project(cv-mmap)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(CVMMAP_BUILD_TESTS "Build cv-mmap test targets" OFF)
if(CVMMAP_BUILD_TESTS)
	enable_testing()
endif()

# brew install cppzmq
# or
# apt-get install libzmq3-dev
find_package(cppzmq REQUIRED)
find_package(fmt REQUIRED)
find_package(Protobuf REQUIRED)

set(APP_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/app/lib)

# nats.c: disable streaming and examples, temporarily disable testing
set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)
set(NATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING_SAVED ${BUILD_TESTING})
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/nats.c)
if(TARGET nats_static AND NOT TARGET cnats::nats_static)
	add_library(cnats::nats_static ALIAS nats_static)
endif()
set(BUILD_TESTING ${BUILD_TESTING_SAVED} CACHE BOOL "" FORCE)

add_subdirectory(${APP_LIB_DIR}/CLI11)
add_subdirectory(core)


add_executable(cv-mmap src/main.cpp)
target_include_directories(cv-mmap PRIVATE src/inc)
target_link_libraries(cv-mmap cppzmq)
target_link_libraries(cv-mmap CLI11::CLI11 spdlog)
target_link_options(cv-mmap PRIVATE "LINKER:--copy-dt-needed-entries")

add_subdirectory(app)
target_link_libraries(cv-mmap CLI11::CLI11 spdlog fmt::fmt)
target_link_libraries(cv-mmap app)
target_link_libraries(cv-mmap cvmmap::nats)
