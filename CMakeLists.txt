cmake_minimum_required(VERSION 3.20)
project(cv-mmap)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(CVMMAP_BUILD_TESTS "Build cv-mmap test targets" OFF)
if(CVMMAP_BUILD_TESTS)
	enable_testing()
endif()

include(CheckCXXSourceCompiles)

set(CVMMAP_HAS_STD_EXPECTED 0)
set(CVMMAP_HAS_STD_FORMAT 0)

set(_CVMMAP_REQUIRED_FLAGS_SAVED "${CMAKE_REQUIRED_FLAGS}")
if(DEFINED CMAKE_CXX23_STANDARD_COMPILE_OPTION)
	string(APPEND CMAKE_REQUIRED_FLAGS " ${CMAKE_CXX23_STANDARD_COMPILE_OPTION}")
endif()

check_cxx_source_compiles(
	"#include <expected>
	int main() {
		[[maybe_unused]] std::expected<int, int> value{42};
		[[maybe_unused]] auto error = std::unexpected(7);
		return value.value();
	}"
	CVMMAP_HAS_STD_EXPECTED)

check_cxx_source_compiles(
	"#include <format>
	#include <string>
	int main() {
		return std::format(\"{}\", 42) == std::string{\"42\"} ? 0 : 1;
	}"
	CVMMAP_HAS_STD_FORMAT)

set(CMAKE_REQUIRED_FLAGS "${_CVMMAP_REQUIRED_FLAGS_SAVED}")
unset(_CVMMAP_REQUIRED_FLAGS_SAVED)

if(CVMMAP_HAS_STD_EXPECTED)
	set(CVMMAP_HAS_STD_EXPECTED 1)
else()
	set(CVMMAP_HAS_STD_EXPECTED 0)
endif()

if(CVMMAP_HAS_STD_FORMAT)
	set(CVMMAP_HAS_STD_FORMAT 1)
else()
	set(CVMMAP_HAS_STD_FORMAT 0)
endif()

message(STATUS "cv-mmap native std::expected support: ${CVMMAP_HAS_STD_EXPECTED}")
message(STATUS "cv-mmap native std::format support: ${CVMMAP_HAS_STD_FORMAT}")

# brew install cppzmq
# or
# apt-get install libzmq3-dev
find_package(cppzmq REQUIRED)
find_package(Protobuf REQUIRED)
if(Protobuf_VERSION VERSION_LESS 3.13)
	set(_cvmmap_protoc_wrapper "${CMAKE_BINARY_DIR}/cvmmap-protoc-wrapper.sh")
	file(WRITE "${_cvmmap_protoc_wrapper}"
"	#!/bin/sh\nexec \"${Protobuf_PROTOC_EXECUTABLE}\" --experimental_allow_proto3_optional \"$@\"\n")
	file(CHMOD
		"${_cvmmap_protoc_wrapper}"
		PERMISSIONS
			OWNER_READ OWNER_WRITE OWNER_EXECUTE
			GROUP_READ GROUP_EXECUTE
			WORLD_READ WORLD_EXECUTE)
	if(TARGET protobuf::protoc)
		set_target_properties(protobuf::protoc PROPERTIES IMPORTED_LOCATION "${_cvmmap_protoc_wrapper}")
	endif()
	set(Protobuf_PROTOC_EXECUTABLE "${_cvmmap_protoc_wrapper}")
endif()

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
		app
		cvmmap::nats)