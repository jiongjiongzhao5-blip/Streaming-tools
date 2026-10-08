cmake_minimum_required(VERSION 3.21)

project(LightLiveStreamerLearn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)

if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
  add_compile_options(/utf-8)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Qt6 REQUIRED COMPONENTS Widgets)

find_package(FFMPEG REQUIRED)

message(STATUS "Qt6 版本: ${Qt6_VERSION}")
message(STATUS "FFMPEG 头文件: ${FFMPEG_INCLUDE_DIRS}")

add_subdirectory(src/core/common)
add_subdirectory(src/app)
