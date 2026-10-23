# The GUI's logic, free of any window or widget, and its tests.
add_subdirectory(core)
if(KUSTAVI_BUILD_TESTS)
  add_subdirectory(testing)
  add_subdirectory(tests)
endif()

if(NOT KUSTAVI_BUILD_GUI)
  return()
endif()

add_executable(kustavi_gui_spike main.cpp thumbnail_loader.cpp)
target_link_libraries(kustavi_gui_spike PRIVATE kustavi_options basalt::basalt magma::magma
  opencv_core opencv_imgproc opencv_imgcodecs)
target_compile_definitions(kustavi_gui_spike PRIVATE
  KUSTAVI_PHOTO_DIR="${PROJECT_SOURCE_DIR}/test/photos")
kustavi_opencv_includes(kustavi_gui_spike)
