cmake_minimum_required(VERSION 4.3)
project(Tests)

file(GLOB SOURCES CONFIGURE_DEPENDS main.cpp "*/*.cc")
add_executable(Tests ${SOURCES})

target_link_libraries(Tests GTest::gtest GTest::gmock Glove::E37)

include(GoogleTest)
gtest_discover_tests(Tests)
