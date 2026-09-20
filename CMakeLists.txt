cmake_minimum_required(VERSION 4.3)
project(Glove LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

if (NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "This project requires the Ninja generator. Refers to https://cmake.org/cmake/help/latest/manual/cmake-cxxmodules.7.html#generator-support")
endif ()

# Get current system time and print the build time
execute_process(COMMAND "date" +"%Y-%m-%d %H:%M.%S" OUTPUT_VARIABLE CURRENT_SYS_TIME)
string(REGEX REPLACE "\n" "" CURRENT_SYS_TIME ${CURRENT_SYS_TIME})
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

# Get git information. WARNING: For functions which invoke execute_process, the variable populated by execute_process is visiable only inside the function!
find_package(Git)
if (NOT Git_FOUND)
    message(FATAL_ERROR "Git not found.")
endif ()
execute_process(
        COMMAND "${GIT_EXECUTABLE}" symbolic-ref --short HEAD
        OUTPUT_VARIABLE GIT_BRANCH_NAME
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-list --count "${GIT_TAG}..HEAD"
        OUTPUT_VARIABLE COMMIT_COUNT
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        OUTPUT_VARIABLE HEAD_COMMIT_ID
        OUTPUT_STRIP_TRAILING_WHITESPACE)

set(GIT_COMMIT_ID "${GIT_TAG}-${COMMIT_COUNT}-${HEAD_COMMIT_ID}")
if ("${GIT_BRANCH_NAME}" STREQUAL "")
    message(WARNING "Branch name not found.")
else ()
    message(STATUS "Branch name = ${GIT_BRANCH_NAME}")
endif ()
if ("${HEAD_COMMIT_ID}" STREQUAL "")
    message(WARNING "Commit id not found.")
else ()
    message(STATUS "Commit-id = ${GIT_COMMIT_ID}")
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

message(STATUS "Build type = ${CMAKE_BUILD_TYPE}")

#add_compile_options(
#        -Wall
#        -Werror
#        -Wno-asm-operand-widths
#        -Wno-unused-command-line-argument
#        -Wno-deprecated-declarations
#        -Wextra
#        -Wno-unused-parameter
#        -Wno-unused-private-field
#        -fcolor-diagnostics
#)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror")
string(APPEND CMAKE_CXX_FLAGS " -Wno-asm-operand-widths")
string(APPEND CMAKE_CXX_FLAGS " -Wno-unused-command-line-argument")
string(APPEND CMAKE_CXX_FLAGS " -Wno-deprecated-declarations")
string(APPEND CMAKE_CXX_FLAGS " -Wextra -Wno-unused-parameter")
string(APPEND CMAKE_CXX_FLAGS " -Wno-unused-private-field")
string(APPEND CMAKE_CXX_FLAGS " -fcolor-diagnostics")

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror -Wno-asm-operand-widths -Wno-unused-command-line-argument -Wno-deprecated-declarations -Wextra -Wno-unused-parameter -Wno-unused-private-field -pthread -fcolor-diagnostics")

MESSAGE(STATUS "C++ Compilation flags: " ${CMAKE_CXX_FLAGS})

add_subdirectory(external EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)

add_subdirectory(benchmark)


