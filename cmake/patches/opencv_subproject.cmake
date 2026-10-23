# Lets OpenCV build as a subproject. Its mlas sources locate files through
# CMAKE_SOURCE_DIR, which only equals OpenCV's tree when OpenCV is the
# top-level project. Run from OpenCV's source directory.
set(file 3rdparty/mlas/CMakeLists.txt)
file(READ ${file} content)
string(REPLACE "\${CMAKE_SOURCE_DIR}" "\${OpenCV_SOURCE_DIR}" content "${content}")
file(WRITE ${file} "${content}")

# The protobuf target comes from another subproject, so it cannot be part of
# OpenCV's install/export sets. Link it for the build tree only.
set(file modules/dnn/CMakeLists.txt)
file(READ ${file} content)
string(REPLACE "list(APPEND libs \${Protobuf_LIBRARIES})"
       "list(APPEND libs \"\$<BUILD_INTERFACE:\${Protobuf_LIBRARIES}>\")"
       content "${content}")
file(WRITE ${file} "${content}")

# Nothing consumes OpenCV's build-tree package config; drop its export.
set(file cmake/OpenCVGenConfig.cmake)
file(READ ${file} content)
string(REPLACE "export(EXPORT OpenCVModules FILE \"\${CMAKE_BINARY_DIR}/OpenCVModules.cmake\")"
       "# export(EXPORT OpenCVModules) disabled for subproject builds"
       content "${content}")
file(WRITE ${file} "${content}")

# With PROTOBUF_UPDATE_FILES the generated *.pb.h land in the module's binary
# directory, which the module does not add to its include path.
set(file modules/dnn/CMakeLists.txt)
file(READ ${file} content)
string(REPLACE "list(APPEND include_dirs \${fw_inc})"
       "list(APPEND fw_inc \${CMAKE_CURRENT_BINARY_DIR})\nlist(APPEND include_dirs \${fw_inc})"
       content "${content}")
file(WRITE ${file} "${content}")

# OpenCV 5.0.0 ships only pre-generated Caffe sources (for protobuf 3.19) and
# omits the schema, which caffe_io.hpp needs. Add the schema from OpenCV 4.x
# (cmake/patches/opencv-caffe.proto) so it is regenerated like the others.
file(COPY ${CMAKE_CURRENT_LIST_DIR}/opencv-caffe.proto
     DESTINATION modules/dnn/src/caffe)
set(file modules/dnn/CMakeLists.txt)
file(READ ${file} content)
string(REPLACE "src/onnx/opencv-onnx.proto\"" "src/onnx/opencv-onnx.proto\" \"\${CMAKE_CURRENT_LIST_DIR}/src/caffe/opencv-caffe.proto\"" content "${content}")
file(WRITE ${file} "${content}")

# OpenCV 5.0.0's MLAS declares MlasHGemmSupported but defines it only in the
# half-precision GEMM sources, which the GEMM-only build leaves out. Report no
# half-precision support so the FP16 attention path stays unused.
set(file 3rdparty/mlas/lib/compute.cpp)
file(READ ${file} content)
if(NOT content MATCHES "kustavi: MlasHGemmSupported")
  string(APPEND content "
// kustavi: MlasHGemmSupported
bool
MLASCALL
MlasHGemmSupported(CBLAS_TRANSPOSE, CBLAS_TRANSPOSE)
{
    return false;
}
")
  file(WRITE ${file} "${content}")
endif()
