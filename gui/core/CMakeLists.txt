add_library(kustavi_gui_core STATIC
  backend_error.cpp
  batches.cpp
  deletion.cpp
  domain.cpp
  format.cpp
  model_prep.cpp
  neighbors.cpp
  ring_buffer.cpp
  trip_edits.cpp
  trips_plan.cpp)
add_library(kustavi::gui_core ALIAS kustavi_gui_core)
# Sources include each other as "core/<name>.h".
target_include_directories(kustavi_gui_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kustavi_gui_core
  PUBLIC kustavi::proto gRPC::grpc++
  PRIVATE kustavi_options nlohmann_json::nlohmann_json)
