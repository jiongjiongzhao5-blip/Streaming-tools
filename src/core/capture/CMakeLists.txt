add_library(core_capture STATIC
  ScreenCapturer.cpp
  ScreenCapturer.h
  CameraCapturer.cpp
  CameraCapturer.h
)

target_include_directories(core_capture PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(core_capture PUBLIC
  core_common
)

if(WIN32)
  target_link_libraries(core_capture PUBLIC
    d3d11
    dxgi
    gdi32
    user32
    strmiids
    ole32
    oleaut32
  )
endif()
