add_executable(LightLiveStreamer
  main.cpp
  MainWindow.cpp
  MainWindow.h
  PreviewWidget.cpp
  PreviewWidget.h
)

target_link_libraries(LightLiveStreamer PRIVATE
  core_pipeline
  Qt6::Widgets
)

set_target_properties(LightLiveStreamer PROPERTIES WIN32_EXECUTABLE TRUE)
