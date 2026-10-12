add_executable(demo1_mini_pusher
  main.cpp
)

target_include_directories(demo1_mini_pusher PRIVATE
  ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(demo1_mini_pusher PRIVATE
  ${FFMPEG_LIBRARIES}
)
