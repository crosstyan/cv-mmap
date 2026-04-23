cmake_minimum_required(VERSION 3.20)
project(cv-mmap)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(GNUInstallDirs)
option(CVMMAP_BUILD_TESTS "Build cv-mmap test targets" OFF)
if(CVMMAP_BUILD_TESTS)
	enable_testing()
endif()

function(cvmmap_check_cxx23_source_compiles output_var source_name source_content)
	try_compile(${output_var}
		SOURCE_FROM_CONTENT "${source_name}" "${source_content}"
		CMAKE_FLAGS
			"-DCMAKE_CXX_STANDARD=23"
			"-DCMAKE_CXX_STANDARD_REQUIRED=ON"
			"-DCMAKE_CXX_EXTENSIONS=OFF")
	set(${output_var} "${${output_var}}" PARENT_SCOPE)
endfunction()

set(CVMMAP_HAS_STD_EXPECTED 0)
set(CVMMAP_HAS_STD_FORMAT 0)
cvmmap_check_cxx23_source_compiles(
	CVMMAP_HAS_STD_EXPECTED
	"cvmmap_check_expected.cpp"
	"#include <expected>
	int main() {
		[[maybe_unused]] std::expected<int, int> value{42};
		[[maybe_unused]] auto error = std::unexpected(7);
		return value.value();
	}")

cvmmap_check_cxx23_source_compiles(
	CVMMAP_HAS_STD_FORMAT
	"cvmmap_check_format.cpp"
	"#include <format>
	#include <string>
	int main() {
		return std::format(\"{}\", 42) == std::string{\"42\"} ? 0 : 1;
	}")

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

function(cvmmap_append_runtime_dir out_var runtime_library)
	execute_process(
		COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=${runtime_library}
		OUTPUT_VARIABLE runtime_library_path
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	if (IS_ABSOLUTE "${runtime_library_path}")
		get_filename_component(runtime_library_dir "${runtime_library_path}" DIRECTORY)
		set(${out_var} "${${out_var}};${runtime_library_dir}" PARENT_SCOPE)
	endif()
endfunction()

set(CVMMAP_BUILD_RPATH "")
# Build-tree binaries compiled with a non-system GCC need that toolchain's
# C++ runtime on the dynamic loader search path.
cvmmap_append_runtime_dir(CVMMAP_BUILD_RPATH libstdc++.so.6)
cvmmap_append_runtime_dir(CVMMAP_BUILD_RPATH libgcc_s.so.1)
list(REMOVE_DUPLICATES CVMMAP_BUILD_RPATH)


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

add_executable(cv-mmap src/main.cpp src/frame_publisher.cpp)
target_include_directories(cv-mmap PRIVATE src/inc)
target_link_libraries(cv-mmap PRIVATE cppzmq)
target_link_options(cv-mmap PRIVATE "LINKER:--copy-dt-needed-entries")
if (CVMMAP_BUILD_RPATH)
	set_target_properties(cv-mmap PROPERTIES
		BUILD_RPATH "${CVMMAP_BUILD_RPATH}")
endif()
set_target_properties(cv-mmap PROPERTIES
	INSTALL_RPATH "$ORIGIN/../lib")

add_subdirectory(app)
target_link_libraries(cv-mmap
	PRIVATE
		CLI11::CLI11
		spdlog::spdlog
		app
		cvmmap::nats)


install(TARGETS cv-mmap
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})