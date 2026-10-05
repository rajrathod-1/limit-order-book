# GoogleTest and Google Benchmark.
#
# Preference order: a system package if one is installed, otherwise fetch a
# pinned release. Pinning matters for a benchmark project -- an unpinned
# dependency means a number measured today is not comparable to one measured
# next month.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(LOB_BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.15.2
            GIT_SHALLOW    TRUE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

if(LOB_BUILD_BENCHMARKS)
    find_package(benchmark QUIET)
    if(NOT benchmark_FOUND)
        FetchContent_Declare(googlebenchmark
            GIT_REPOSITORY https://github.com/google/benchmark.git
            GIT_TAG        v1.9.0
            GIT_SHALLOW    TRUE)
        set(BENCHMARK_ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL     OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_INSTALL_DOCS       OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googlebenchmark)
    endif()
endif()
