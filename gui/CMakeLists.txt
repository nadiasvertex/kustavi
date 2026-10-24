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

# The application: screens built from the core's state, and the program that runs them.
add_library(kustavi_gui_app STATIC
  app/executable_path.cpp
  app/image_grid.cpp
  app/image_loader.cpp
  app/screens.cpp
  app/trips_screen.cpp
  app/wizard_shell.cpp)
target_include_directories(kustavi_gui_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kustavi_gui_app PUBLIC kustavi::gui_core basalt::basalt magma::magma
  PRIVATE kustavi_options opencv_core opencv_imgproc opencv_imgcodecs)
kustavi_opencv_includes(kustavi_gui_app)

add_executable(kustavi_gui app/main.cpp)
target_link_libraries(kustavi_gui PRIVATE kustavi_gui_app kustavi_options)
target_compile_definitions(kustavi_gui PRIVATE KUSTAVI_VERSION="${PROJECT_VERSION}")

# Draws a screen to a PNG with a scripted back end, for a look at the layout.
add_executable(kustavi_gui_preview app/preview.cpp)
target_link_libraries(kustavi_gui_preview PRIVATE kustavi_gui_app kustavi::gui_testing
  kustavi_options opencv_core opencv_imgproc opencv_imgcodecs)
target_compile_definitions(kustavi_gui_preview PRIVATE
  KUSTAVI_PHOTO_DIR="${PROJECT_SOURCE_DIR}/test/photos")
kustavi_opencv_includes(kustavi_gui_preview)

if(KUSTAVI_BUILD_TESTS)
  add_executable(gui_app_tests tests/app_test.cpp)
  target_link_libraries(gui_app_tests PRIVATE kustavi_gui_app kustavi::gui_testing kustavi_options
    Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(gui_app_tests PROPERTIES LABELS gui)
endif()
