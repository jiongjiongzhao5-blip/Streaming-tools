add_executable(test_common
  test_common.cpp
)

target_link_libraries(test_common PRIVATE
  core_common
)

add_executable(test_screen_capture
  test_screen_capture.cpp
)

target_link_libraries(test_screen_capture PRIVATE
  core_capture
)

add_executable(test_camera_capture
  test_camera_capture.cpp
)

target_link_libraries(test_camera_capture PRIVATE
  core_capture
)

add_executable(test_camera_convert
  test_camera_convert.cpp
)

target_link_libraries(test_camera_convert PRIVATE
  core_capture
)

add_executable(test_audio_capture
  test_audio_capture.cpp
)

target_link_libraries(test_audio_capture PRIVATE
  core_capture
  winmm
)
