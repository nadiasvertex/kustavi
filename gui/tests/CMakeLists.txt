add_executable(gui_core_tests
  batches_test.cpp
  deletion_test.cpp
  domain_test.cpp
  format_test.cpp
  model_prep_test.cpp
  neighbors_test.cpp
  ring_buffer_test.cpp
  trip_edits_test.cpp
  trips_plan_test.cpp)
target_link_libraries(gui_core_tests PRIVATE kustavi::gui_core kustavi_options
  Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(gui_core_tests PROPERTIES LABELS gui)
