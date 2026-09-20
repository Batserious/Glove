cmake_minimum_required(VERSION 4.3)
project(Tests)

file(GLOB SOURCES CONFIGURE_DEPENDS "*.cpp")
add_executable(Tests ${SOURCES})

find_package(GTest REQUIRED)
target_link_libraries(Tests GTest::gtest GTest::gmock)

