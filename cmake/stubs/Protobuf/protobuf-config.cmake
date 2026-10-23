# Lets find_package(Protobuf CONFIG) succeed for OpenCV using the protobuf that
# gRPC builds in this same configure. OpenCV's dnn module regenerates its
# ONNX/TensorFlow/Caffe sources against it, so the process holds one protobuf
# runtime instead of two. KUSTAVI_PROTOBUF_SOURCE_DIR is set in
# dependencies.cmake.
set(Protobuf_FOUND TRUE)
set(Protobuf_VERSION 99.0.0)
set(Protobuf_INCLUDE_DIR ${KUSTAVI_PROTOBUF_SOURCE_DIR}/src)
set(Protobuf_INCLUDE_DIRS ${Protobuf_INCLUDE_DIR})
set(Protobuf_PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
include(${KUSTAVI_PROTOBUF_SOURCE_DIR}/cmake/protobuf-generate.cmake)

# OpenCV only checks that this command exists; with CMake >= 3.13 it calls
# protobuf_generate instead.
if(NOT COMMAND PROTOBUF_GENERATE_CPP)
  function(PROTOBUF_GENERATE_CPP)
    message(FATAL_ERROR "PROTOBUF_GENERATE_CPP is not supported by this stub")
  endfunction()
endif()
