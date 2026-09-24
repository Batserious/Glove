# Dependencies found here are linked by sibling project directories.
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

find_package(GTest QUIET)
if (NOT TARGET GTest::gtest OR NOT TARGET GTest::gmock)
    set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of bundled GoogleTest" FORCE)
    add_subdirectory(GoogleTest EXCLUDE_FROM_ALL)
endif ()

find_package(benchmark QUIET)
if (NOT TARGET benchmark::benchmark)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL
            "Disable tests for bundled Google Benchmark" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL
            "Disable installation of bundled Google Benchmark" FORCE)
    add_subdirectory(GoogleBenchmark EXCLUDE_FROM_ALL)
endif ()

find_package(spdlog QUIET)
if (NOT TARGET spdlog::spdlog)
    add_subdirectory(Spdlog EXCLUDE_FROM_ALL)
endif ()