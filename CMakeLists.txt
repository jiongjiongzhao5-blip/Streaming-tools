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

if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
  list(PREPEND CMAKE_MODULE_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/ffmpeg")
else()
  list(PREPEND CMAKE_MODULE_PATH "C:/vcpkg/installed/x64-windows/share/ffmpeg")
endif()

find_package(FFMPEG REQUIRED)

message(STATUS "Qt6 版本: ${Qt6_VERSION}")
message(STATUS "FFMPEG 头文件: ${FFMPEG_INCLUDE_DIRS}")

add_subdirectory(src/core/common)
add_subdirectory(src/core/capture)
add_subdirectory(src/core/encode)
add_subdirectory(demo/01-mini-pusher)
add_subdirectory(src/app)
add_subdirectory(tests)
