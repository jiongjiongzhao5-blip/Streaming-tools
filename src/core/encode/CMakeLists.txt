add_library(core_encode STATIC
  VideoEncoder.cpp
  VideoEncoder.h
  AudioEncoder.cpp
  AudioEncoder.h
  EncoderCommon.h
)

target_include_directories(core_encode
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..
  PRIVATE ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(core_encode PUBLIC
  core_common
  ${FFMPEG_LIBRARIES}
)
