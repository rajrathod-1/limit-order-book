# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)

project(lob
    VERSION 1.0.0
    DESCRIPTION "Price-time priority limit order book and matching engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
    message(STATUS "No build type specified; defaulting to Release")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LOB_BUILD_TESTS      "Build the test suite"          ON)
option(LOB_BUILD_BENCHMARKS "Build the Google Benchmark suite" ON)
option(LOB_NATIVE_ARCH      "Tune for the building machine" ON)
option(LOB_ENABLE_ASAN      "Build with address+UB sanitizers" OFF)

include(cmake/CompilerWarnings.cmake)
include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# The book itself is header-only: everything is templates on the engine and the
# event sink, so an application compiles exactly the specialisation it uses and
# nothing is dispatched through a vtable on the hot path.
# ---------------------------------------------------------------------------
add_library(lob INTERFACE)
add_library(lob::lob ALIAS lob)
target_include_directories(lob INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(lob INTERFACE cxx_std_20)

# ITCH decoding needs zlib to stream Nasdaq's gzipped day files.
find_package(ZLIB REQUIRED)

add_library(itch INTERFACE)
add_library(lob::itch ALIAS itch)
target_include_directories(itch INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(itch INTERFACE lob::lob ZLIB::ZLIB)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
add_executable(itch_filter apps/itch_filter.cpp)
target_link_libraries(itch_filter PRIVATE lob::itch)
lob_set_warnings(itch_filter)
lob_tune(itch_filter)

add_executable(itch_gen apps/itch_gen.cpp)
target_link_libraries(itch_gen PRIVATE lob::itch)
lob_set_warnings(itch_gen)
lob_tune(itch_gen)

add_executable(lob_replay apps/lob_replay.cpp)
target_link_libraries(lob_replay PRIVATE lob::itch)
lob_set_warnings(lob_replay)
lob_tune(lob_replay)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LOB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(LOB_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

message(STATUS "")
message(STATUS "lob ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  tests      : ${LOB_BUILD_TESTS}")
message(STATUS "  benchmarks : ${LOB_BUILD_BENCHMARKS}")
message(STATUS "  native arch: ${LOB_NATIVE_ARCH}")
message(STATUS "  sanitizers : ${LOB_ENABLE_ASAN}")
message(STATUS "")
