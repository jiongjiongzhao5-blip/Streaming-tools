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

install(TARGETS LightLiveStreamer RUNTIME DESTINATION .)

install(CODE "
  file(GLOB _root_dlls \"${CMAKE_BINARY_DIR}/*.dll\")
  file(COPY \${_root_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
")

foreach(_plugin_dir platforms styles tls imageformats iconengines generic networkinformation)
  if(EXISTS "${CMAKE_BINARY_DIR}/${_plugin_dir}")
    install(DIRECTORY "${CMAKE_BINARY_DIR}/${_plugin_dir}"
            DESTINATION . FILES_MATCHING PATTERN "*.dll")
  endif()
endforeach()
