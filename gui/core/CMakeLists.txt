add_library(kustavi_gui_core STATIC
  backend_error.cpp
  backend_launch.cpp
  batches.cpp
  deletion.cpp
  domain.cpp
  format.cpp
  grpc_kustavi_client.cpp
  kustavi_client.cpp
  model_prep.cpp
  neighbors.cpp
  trip_edits.cpp
  trips_plan.cpp
  ui_queue.cpp)
add_library(kustavi::gui_core ALIAS kustavi_gui_core)
# Sources include each other as "core/<name>.h".
target_include_directories(kustavi_gui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kustavi_gui_core
  PUBLIC kustavi::proto kustavi::process gRPC::grpc++
  PRIVATE kustavi_options nlohmann_json::nlohmann_json)
