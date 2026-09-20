cmake_minimum_required(VERSION 4.3)
project(Benches)

file(GLOB SOURCES CONFIGURE_DEPENDS "*.cpp")
add_executable(Benches ${SOURCES})

target_link_libraries(Benches benchmark::benchmark)

