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

add_subdirectory(third_party)
add_subdirectory(core)


add_executable(cv-mmap src/main.cpp)
target_include_directories(cv-mmap PRIVATE src/inc)
target_link_libraries(cv-mmap PRIVATE cppzmq)
target_link_options(cv-mmap PRIVATE "LINKER:--copy-dt-needed-entries")

add_subdirectory(app)
target_link_libraries(cv-mmap
	PRIVATE
		CLI11::CLI11
		spdlog::spdlog
		fmt::fmt
		app
		cvmmap::nats)
