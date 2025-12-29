cmake_minimum_required(VERSION 3.20)
project(cv-mmap)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# brew install cppzmq
# or
# apt-get install libzmq3-dev
find_package(cppzmq REQUIRED)
set(APP_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/app/lib)

add_subdirectory(${APP_LIB_DIR}/CLI11)


add_executable(cv-mmap src/main.cpp)
target_include_directories(cv-mmap PRIVATE src/inc)
target_include_directories(cv-mmap PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(cv-mmap ${OpenCV_LIBS} cppzmq)
target_link_libraries(cv-mmap CLI11::CLI11 spdlog)
target_link_options(cv-mmap PRIVATE "LINKER:--copy-dt-needed-entries")

add_subdirectory(app)
target_link_libraries(cv-mmap app)
