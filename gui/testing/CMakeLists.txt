# Test doubles for the GUI core, linked by tests only.
add_library(kustavi_gui_testing STATIC fake_kustavi_client.cpp)
add_library(kustavi::gui_testing ALIAS kustavi_gui_testing)
target_include_directories(kustavi_gui_testing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kustavi_gui_testing PUBLIC kustavi::gui_core PRIVATE kustavi_options)
