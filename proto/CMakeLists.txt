# C++ protobuf and gRPC sources for service.proto, shared by the back end and
# the GUI client. Headers are included as <proto/service.grpc.pb.h>.

# The plugin target is gRPC::grpc_cpp_plugin when gRPC is installed and
# grpc_cpp_plugin when it is built as a subproject.
if(TARGET gRPC::grpc_cpp_plugin)
  set(KUSTAVI_GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin)
else()
  set(KUSTAVI_GRPC_CPP_PLUGIN grpc_cpp_plugin)
endif()

set(proto_file ${CMAKE_CURRENT_SOURCE_DIR}/service.proto)
set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${gen_dir}/proto)

set(gen_sources
  ${gen_dir}/proto/service.pb.cc ${gen_dir}/proto/service.pb.h
  ${gen_dir}/proto/service.grpc.pb.cc ${gen_dir}/proto/service.grpc.pb.h)

add_custom_command(
  OUTPUT ${gen_sources}
  COMMAND $<TARGET_FILE:protobuf::protoc>
          --proto_path=${PROJECT_SOURCE_DIR}
          --cpp_out=${gen_dir}
          --grpc_out=${gen_dir}
          --plugin=protoc-gen-grpc=$<TARGET_FILE:${KUSTAVI_GRPC_CPP_PLUGIN}>
          proto/service.proto
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  DEPENDS ${proto_file} protobuf::protoc ${KUSTAVI_GRPC_CPP_PLUGIN}
  COMMENT "Generating C++ sources for service.proto"
  VERBATIM)

add_library(kustavi_proto STATIC ${gen_sources})
target_include_directories(kustavi_proto SYSTEM PUBLIC ${gen_dir})
target_link_libraries(kustavi_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
set_target_properties(kustavi_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(kustavi::proto ALIAS kustavi_proto)
