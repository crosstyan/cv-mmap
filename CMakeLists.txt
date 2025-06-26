cmake_minimum_required(VERSION 3.20)
project(cv-mmap)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(OpenCV REQUIRED)

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

add_executable(cv-mmap src/main.cpp)
target_include_directories(cv-mmap PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(cv-mmap ${OpenCV_LIBS} cppzmq)
target_link_libraries(cv-mmap CLI11::CLI11 tomlplusplus::tomlplusplus spdlog)
target_link_options(cv-mmap PRIVATE "LINKER:--copy-dt-needed-entries")

add_subdirectory(app)
target_link_libraries(cv-mmap app)
