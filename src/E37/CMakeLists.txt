cmake_minimum_required(VERSION 4.3)
project(E37)

file(GLOB SOURCES CONFIGURE_DEPENDS main.cpp "src/*.cpp")

add_library(${PROJECT_NAME} ${SOURCES})
add_library(Glove::E37 ALIAS E37)

target_include_directories(E37 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(E37 PUBLIC cxx_std_23)

target_link_libraries(E37 PUBLIC asio::asio PRIVATE spdlog::spdlog)
