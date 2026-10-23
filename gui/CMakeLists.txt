add_executable(kustavi_gui_spike main.cpp thumbnail_loader.cpp)
target_link_libraries(kustavi_gui_spike PRIVATE kustavi_options basalt::basalt magma::magma
  opencv_core opencv_imgproc opencv_imgcodecs)
target_compile_definitions(kustavi_gui_spike PRIVATE
  KUSTAVI_PHOTO_DIR="${PROJECT_SOURCE_DIR}/test/photos")
kustavi_opencv_includes(kustavi_gui_spike)
