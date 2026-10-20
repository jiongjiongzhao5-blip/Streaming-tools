add_library(core_output STATIC
  StreamOutput.cpp
  StreamOutput.h
)

target_include_directories(core_output
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..
  PRIVATE ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(core_output PUBLIC
  core_common
  ${FFMPEG_LIBRARIES}
)
