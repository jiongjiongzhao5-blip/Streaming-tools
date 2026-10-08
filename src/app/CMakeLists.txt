add_executable(LightLiveStreamer
  main.cpp
)

target_include_directories(LightLiveStreamer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(LightLiveStreamer PRIVATE
  core_common
  Qt6::Widgets
  ${FFMPEG_LIBRARIES}
)
