add_library(core_compose STATIC
  VideoCompositor.cpp
  VideoCompositor.h
)

target_include_directories(core_compose
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..
  PRIVATE ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(core_compose PUBLIC
  core_common
  ${FFMPEG_LIBRARIES}
)

if(WIN32)
  target_link_libraries(core_compose PUBLIC gdi32 user32)
endif()
