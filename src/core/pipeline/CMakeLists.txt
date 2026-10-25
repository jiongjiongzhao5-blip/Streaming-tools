add_library(core_pipeline STATIC
  StreamPipeline.cpp
  StreamPipeline.h
)

target_include_directories(core_pipeline
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..
  PRIVATE ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(core_pipeline PUBLIC
  core_common
  core_capture
  core_compose
  core_encode
  core_output
)

if(WIN32)
  target_link_libraries(core_pipeline PUBLIC winmm)
endif()
