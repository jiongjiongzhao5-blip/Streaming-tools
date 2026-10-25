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

add_executable(test_encode
  test_encode.cpp
)

target_link_libraries(test_encode PRIVATE
  core_encode
)

add_executable(test_output
  test_output.cpp
)

target_link_libraries(test_output PRIVATE
  core_encode
  core_output
)

add_executable(test_compose
  test_compose.cpp
)

target_link_libraries(test_compose PRIVATE
  core_compose
)

add_executable(test_pipeline
  test_pipeline.cpp
)

target_link_libraries(test_pipeline PRIVATE
  core_pipeline
)

add_executable(test_audio_capture
  test_audio_capture.cpp
)

target_link_libraries(test_audio_capture PRIVATE
  core_capture
  winmm
)
