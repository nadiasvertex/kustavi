add_executable(gui_core_tests
  background_test.cpp
  batches_test.cpp
  wizard_batches_test.cpp
  wizard_commit_test.cpp
  ui_timers_test.cpp
  deletion_test.cpp
  domain_test.cpp
  fake_client_test.cpp
  format_test.cpp
  model_prep_test.cpp
  model_status_test.cpp
  neighbors_test.cpp
  phases_test.cpp
  trip_edits_test.cpp
  trips_plan_test.cpp
  ui_queue_test.cpp
  wizard_controller_test.cpp)
target_link_libraries(gui_core_tests PRIVATE kustavi::gui_core kustavi::gui_testing kustavi_options
  Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(gui_core_tests PROPERTIES LABELS gui)

# The production client against a real engine, on a copy of test/photos.
add_executable(gui_engine_tests engine_client_test.cpp)
target_link_libraries(gui_engine_tests PRIVATE kustavi::gui_core kustavi_options
  Catch2::Catch2WithMain)
target_compile_definitions(gui_engine_tests PRIVATE
  KUSTAVI_PHOTO_DIR="${PROJECT_SOURCE_DIR}/test/photos")
kustavi_copy_engine_data(gui_engine_tests)
catch_discover_tests(gui_engine_tests PROPERTIES LABELS gui TIMEOUT 300)
