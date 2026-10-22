add_executable(LightLiveStreamer
  main.cpp
  MainWindow.cpp
  MainWindow.h
)

target_link_libraries(LightLiveStreamer PRIVATE
  core_pipeline
  Qt6::Widgets
)
