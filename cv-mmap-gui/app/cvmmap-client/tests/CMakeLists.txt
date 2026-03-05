cmake_minimum_required(VERSION 3.20)
project(protocol_fixture_check CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(protocol_fixture_check
    protocol_fixture_check.cpp
)

# No external dependencies - pure C++23
# Test harness is self-contained with embedded fixture data
