cmake_minimum_required(VERSION 4.3)
project(Benches)

file(GLOB SOURCES CONFIGURE_DEPENDS "*.cpp")
add_executable(Benches ${SOURCES})

find_package(benchmark REQUIRED)
target_link_libraries(Benches benchmark::benchmark)

