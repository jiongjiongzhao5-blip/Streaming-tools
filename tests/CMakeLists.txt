add_executable(test_common
  test_common.cpp
)

target_link_libraries(test_common PRIVATE
  core_common
)
