add_executable(gui_core_tests
  backend_launch_test.cpp
  batches_test.cpp
  deletion_test.cpp
  domain_test.cpp
  fake_client_test.cpp
  format_test.cpp
  model_prep_test.cpp
  neighbors_test.cpp
  trip_edits_test.cpp
  trips_plan_test.cpp
  ui_queue_test.cpp)
target_link_libraries(gui_core_tests PRIVATE kustavi::gui_core kustavi::gui_testing kustavi_options
  Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(gui_core_tests PROPERTIES LABELS gui)

# These run a gRPC server in the test. The sanitized test code and the unsanitized gRPC share
# inline library code, which the sanitizers report at server shutdown, so the sanitize test preset
# leaves this label out.
add_executable(gui_grpc_tests grpc_client_test.cpp)
target_link_libraries(gui_grpc_tests PRIVATE kustavi::gui_core kustavi_options
  Catch2::Catch2WithMain)
catch_discover_tests(gui_grpc_tests PROPERTIES LABELS grpc)
