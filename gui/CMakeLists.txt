# The GUI's logic, free of any window or widget, and its tests.
add_subdirectory(core)
if(KUSTAVI_BUILD_TESTS)
  add_subdirectory(testing)
  add_subdirectory(tests)
endif()

if(NOT KUSTAVI_BUILD_GUI)
  return()
endif()

# The application: screens built from the core's state, and the program that runs them.
add_library(kustavi_gui_app STATIC
  app/batch_list_item.cpp
  app/batch_menu_screen.cpp
  app/commit_screens.cpp
  app/detail_view.cpp
  app/executable_path.cpp
  app/fonts.cpp
  app/image_grid.cpp
  app/image_loader.cpp
  app/review_screen.cpp
  app/screens.cpp
  app/trips_screen.cpp
  app/video_player.cpp
  app/zoom_view.cpp
  app/wizard_shell.cpp
  app/wrapped_text.cpp)
target_include_directories(kustavi_gui_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kustavi_gui_app PUBLIC kustavi::gui_core basalt::basalt magma::magma
  PRIVATE kustavi_options opencv_core opencv_imgproc opencv_imgcodecs)
kustavi_opencv_includes(kustavi_gui_app)
# A build that runs from the tree reads the fonts from the source.
target_compile_definitions(kustavi_gui_app PUBLIC
  KUSTAVI_FONT_DIR="${PROJECT_SOURCE_DIR}/resources/fonts")

# Video playback uses libmdk, which cmake/gui_dependencies.cmake fetches. Without it the detail view
# shows the still image of a video.
if(KUSTAVI_MDK_DIR AND EXISTS "${KUSTAVI_MDK_DIR}/lib/cmake/FindMDK.cmake")
  list(APPEND CMAKE_MODULE_PATH "${KUSTAVI_MDK_DIR}/lib/cmake")
  find_package(MDK REQUIRED)
  target_link_libraries(kustavi_gui_app PUBLIC mdk)
  if(APPLE)
    # The SDK comes as a framework in `lib`, or as an xcframework with a slice for macOS. A run
    # from the build finds it by this path, and the package copies it beside the program.
    if(EXISTS "${KUSTAVI_MDK_DIR}/lib/mdk.xcframework/macos-arm64_x86_64")
      set(KUSTAVI_MDK_FRAMEWORK_DIR "${KUSTAVI_MDK_DIR}/lib/mdk.xcframework/macos-arm64_x86_64")
    else()
      set(KUSTAVI_MDK_FRAMEWORK_DIR "${KUSTAVI_MDK_DIR}/lib")
    endif()
    set(KUSTAVI_MDK_FRAMEWORK_DIR "${KUSTAVI_MDK_FRAMEWORK_DIR}" CACHE INTERNAL "")
    target_link_options(kustavi_gui_app PUBLIC "-Wl,-rpath,${KUSTAVI_MDK_FRAMEWORK_DIR}")
  endif()
  target_compile_definitions(kustavi_gui_app PRIVATE KUSTAVI_HAVE_MDK)
endif()

add_executable(kustavi_gui app/main.cpp)
target_link_libraries(kustavi_gui PRIVATE kustavi_gui_app kustavi_options)
target_compile_definitions(kustavi_gui PRIVATE KUSTAVI_VERSION="${PROJECT_VERSION}")
kustavi_copy_engine_data(kustavi_gui)

# Draws a screen to a PNG with a scripted back end, for a look at the layout.
add_executable(kustavi_gui_preview app/preview.cpp)
target_link_libraries(kustavi_gui_preview PRIVATE kustavi_gui_app kustavi::gui_testing
  kustavi_options opencv_core opencv_imgproc opencv_imgcodecs)
target_compile_definitions(kustavi_gui_preview PRIVATE
  KUSTAVI_PHOTO_DIR="${PROJECT_SOURCE_DIR}/test/photos")
kustavi_opencv_includes(kustavi_gui_preview)
kustavi_copy_engine_data(kustavi_gui_preview)

if(KUSTAVI_BUILD_TESTS)
  add_executable(gui_app_tests tests/app_test.cpp tests/detail_test.cpp tests/zoom_test.cpp)
  target_compile_definitions(gui_app_tests PRIVATE
    KUSTAVI_PHOTO_DIR="${PROJECT_SOURCE_DIR}/test/photos")
  if(KUSTAVI_MDK_DIR AND EXISTS "${KUSTAVI_MDK_DIR}/lib/cmake/FindMDK.cmake")
    target_sources(gui_app_tests PRIVATE tests/mdk_player_test.cpp)
    target_compile_definitions(gui_app_tests PRIVATE
      KUSTAVI_VIDEO_DIR="${PROJECT_SOURCE_DIR}/test/videos")
  endif()
  target_link_libraries(gui_app_tests PRIVATE kustavi_gui_app kustavi::gui_testing kustavi_options
    Catch2::Catch2WithMain)
  include(Catch)
  catch_discover_tests(gui_app_tests PROPERTIES LABELS gui)
endif()
