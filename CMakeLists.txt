cmake_minimum_required(VERSION 4.3)

set(CMAKE_CXX_STANDARD 23 CACHE STRING "C++ standard to use")
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (CMAKE_EXPORT_COMPILE_COMMANDS STREQUAL "")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
            "Export compile_commands.json for IDEs and tooling" FORCE)
endif ()

if (NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(
        FATAL_ERROR
        "This project requires the Ninja generator. Refers to "
        "https://cmake.org/cmake/help/latest/manual/cmake-cxxmodules.7.html#generator-support")
endif ()

# Use CMake's portable timestamp implementation instead of an external date command.
string(TIMESTAMP CURRENT_SYS_TIME "%Y-%m-%d %H:%M:%S")
message(STATUS "Build time = ${CURRENT_SYS_TIME}")

# Auto-detect and set vcpkg toolchain if not already set
if (NOT CMAKE_TOOLCHAIN_FILE)
    # First try environment variable CMAKE_TOOLCHAIN_FILE (for container builds)
    if (DEFINED ENV{CMAKE_TOOLCHAIN_FILE})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{CMAKE_TOOLCHAIN_FILE}"
                CACHE STRING "Vcpkg toolchain file")
        message(STATUS "Using vcpkg toolchain from environment: ${CMAKE_TOOLCHAIN_FILE}")
        # Fallback to VCPKG_ROOT for backward compatibility
    elseif (DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
                CACHE STRING "Vcpkg toolchain file")
        message(STATUS "Using vcpkg toolchain from VCPKG_ROOT: ${CMAKE_TOOLCHAIN_FILE}")
    endif ()
endif ()

project(Glove LANGUAGES CXX)

# Git metadata is useful in build logs, but should not prevent source archives
# or environments without Git from configuring.
find_package(Git QUIET)
if (Git_FOUND)
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" symbolic-ref --short HEAD
            OUTPUT_VARIABLE GIT_BRANCH_NAME
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
            OUTPUT_VARIABLE GIT_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-list --count "${GIT_TAG}..HEAD"
            OUTPUT_VARIABLE COMMIT_COUNT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            OUTPUT_VARIABLE HEAD_COMMIT_ID
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --short
            OUTPUT_VARIABLE GIT_DIRTY_STATUS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)

    if (GIT_TAG STREQUAL "")
        set(GIT_TAG "untagged")
        set(COMMIT_COUNT "0")
    endif ()
    if (GIT_BRANCH_NAME STREQUAL "")
        set(GIT_BRANCH_NAME "detached")
    endif ()
    if (GIT_DIRTY_STATUS)
        set(GIT_DIRTY_SUFFIX "-dirty")
    endif ()

    set(GIT_COMMIT_ID "${GIT_TAG}-${COMMIT_COUNT}-${HEAD_COMMIT_ID}${GIT_DIRTY_SUFFIX}")
    message(STATUS "Branch = ${GIT_BRANCH_NAME}")
    message(STATUS "Commit = ${GIT_COMMIT_ID}")
else ()
    set(GIT_COMMIT_ID "unknown")
    message(WARNING "Git not found; commit metadata is unavailable.")
endif ()

if (NOT CMAKE_BUILD_TYPE)
    if (EXISTS "${CMAKE_SOURCE_DIR}/.git")
        set(default_build_type "RelWithDebInfo")
    else ()
        set(default_build_type "Debug")
    endif ()

    set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE STRING
            "Default BUILD_TYPE is ${default_build_type}" FORCE)
endif ()

message(STATUS "Configuring external dependencies (this may take a while)...")

# External projects can be verbose during configuration. Keep their
# errors visible while suppressing informational STATUS/NOTICE messages.
set(_glove_message_log_level "${CMAKE_MESSAGE_LOG_LEVEL}")
set(CMAKE_MESSAGE_LOG_LEVEL ERROR)
add_subdirectory(external EXCLUDE_FROM_ALL SYSTEM)
set(CMAKE_MESSAGE_LOG_LEVEL "${_glove_message_log_level}")
unset(_glove_message_log_level)

message(STATUS "External dependencies configured.")

# Keep project diagnostics out of external targets.
if (MSVC)
    add_compile_options(/W4 /WX)
else ()
    add_compile_options(-Wall -Werror -Wextra)
endif ()

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

add_subdirectory(benchmark)

message(STATUS "")
message(STATUS "========== Glove configuration ==========")
message(STATUS "Generator = ${CMAKE_GENERATOR}")
message(STATUS "System = ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Compiler = ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "C++ standard = ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type = ${CMAKE_BUILD_TYPE}")
if (CMAKE_EXPORT_COMPILE_COMMANDS)
    message(STATUS "Compile commands = ${CMAKE_BINARY_DIR}/compile_commands.json")
else ()
    message(STATUS "Compile commands = disabled")
endif ()
message(STATUS "==========================================")

