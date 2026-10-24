# C++ protobuf messages for service.proto, the data that the back end and the GUI exchange.
# Headers are included as <proto/service.pb.h>.

set(proto_file ${CMAKE_CURRENT_SOURCE_DIR}/service.proto)
set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${gen_dir}/proto)

set(gen_sources ${gen_dir}/proto/service.pb.cc ${gen_dir}/proto/service.pb.h)

add_custom_command(
  OUTPUT ${gen_sources}
  COMMAND $<TARGET_FILE:protobuf::protoc>
          --proto_path=${PROJECT_SOURCE_DIR}
          --cpp_out=${gen_dir}
          proto/service.proto
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  DEPENDS ${proto_file} protobuf::protoc
  COMMENT "Generating C++ sources for service.proto"
  VERBATIM)

add_library(kustavi_proto STATIC ${gen_sources})
target_include_directories(kustavi_proto SYSTEM PUBLIC ${gen_dir})
target_link_libraries(kustavi_proto PUBLIC protobuf::libprotobuf)
set_target_properties(kustavi_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(kustavi::proto ALIAS kustavi_proto)
