# Third-party dependencies. Pins live in dependency_pins.cmake. Where a package
# has a CMake config and an installed copy is acceptable, FIND_PACKAGE_ARGS
# lets FetchContent use it before downloading.

include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/dependency_pins.cmake)

set(FETCHCONTENT_QUIET OFF)

# Third-party code is not subject to the repo's warning flags and its headers
# are treated as system headers.
set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)

option(KUSTAVI_USE_SYSTEM_GRPC
       "Use an installed gRPC (and its protobuf) instead of building it" OFF)

# --- TLS ---------------------------------------------------------------------
# One TLS library per process. gRPC and libcurl both use the system OpenSSL on
# macOS and Linux, so two copies of the same symbols are never linked together.
# On Windows curl uses Schannel and gRPC uses its bundled BoringSSL.
# gRPC 1.84 does not build against OpenSSL 4, so prefer 3.x when Homebrew has it.
if(NOT WIN32)
  if(APPLE AND NOT OPENSSL_ROOT_DIR)
    foreach(prefix /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3)
      if(EXISTS ${prefix})
        set(OPENSSL_ROOT_DIR ${prefix})
        break()
      endif()
    endforeach()
  endif()
  set(OPENSSL_USE_STATIC_LIBS ON)
  find_package(OpenSSL 3 REQUIRED)
endif()

# pipe2() is declared by the newest macOS SDK but is not available on the
# deployment target; make c-ares and curl use their pipe() fallback.
if(APPLE)
  set(HAVE_PIPE2 OFF CACHE INTERNAL "")
endif()

# Warnings from third-party code are not ours to fix; relax them for the
# dependencies only. The flags are restored at the end of this file.
set(_kustavi_saved_flags "${CMAKE_C_FLAGS}|${CMAKE_CXX_FLAGS}|${CMAKE_OBJCXX_FLAGS}")
if(NOT MSVC)
  set(_dep_flags "-Wno-error=enum-enum-conversion -Wno-error=enum-compare -Wno-error=ambiguous-reversed-operator -Wno-nullability-extension -Wno-gcc-compat -Wno-deprecated-declarations -Wno-unused-function")
  string(APPEND CMAKE_C_FLAGS " ${_dep_flags}")
  string(APPEND CMAKE_CXX_FLAGS " ${_dep_flags}")
  string(APPEND CMAKE_OBJCXX_FLAGS " ${_dep_flags}")
endif()

# --- gRPC and protobuf -------------------------------------------------------
if(KUSTAVI_USE_SYSTEM_GRPC)
  find_package(gRPC CONFIG REQUIRED)
  find_package(Protobuf CONFIG REQUIRED)
else()
  set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(gRPC_BUILD_CODEGEN ON CACHE BOOL "" FORCE)
  set(gRPC_BUILD_GRPC_CPP_PLUGIN ON CACHE BOOL "" FORCE)
  foreach(lang CSHARP NODE OBJECTIVE_C PHP PYTHON RUBY)
    set(gRPC_BUILD_GRPC_${lang}_PLUGIN OFF CACHE BOOL "" FORCE)
  endforeach()
  set(gRPC_BUILD_CSHARP_EXT OFF CACHE BOOL "" FORCE)
  set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
  set(gRPC_ABSL_PROVIDER module CACHE STRING "" FORCE)
  set(gRPC_PROTOBUF_PROVIDER module CACHE STRING "" FORCE)
  set(gRPC_CARES_PROVIDER module CACHE STRING "" FORCE)
  set(gRPC_RE2_PROVIDER module CACHE STRING "" FORCE)
  # zlib: the system library on macOS and Linux, shared with OpenCV below.
  if(WIN32)
    set(gRPC_ZLIB_PROVIDER module CACHE STRING "" FORCE)
  else()
    set(gRPC_ZLIB_PROVIDER package CACHE STRING "" FORCE)
  endif()
  if(WIN32)
    set(gRPC_SSL_PROVIDER module CACHE STRING "" FORCE)
  else()
    set(gRPC_SSL_PROVIDER package CACHE STRING "" FORCE)
  endif()
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
  set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(grpc
    GIT_REPOSITORY ${KUSTAVI_GRPC_GIT_URL}
    GIT_TAG ${KUSTAVI_GRPC_GIT_TAG}
    GIT_SHALLOW OFF
    GIT_SUBMODULES_RECURSE ON
    GIT_PROGRESS ON
    SYSTEM
    FIND_PACKAGE_ARGS NAMES gRPC)
  FetchContent_MakeAvailable(grpc)
  # Subproject targets are unqualified; give them the installed names.
  add_library(gRPC::grpc++ ALIAS grpc++)
  set(KUSTAVI_PROTOBUF_SOURCE_DIR ${grpc_SOURCE_DIR}/third_party/protobuf
      CACHE INTERNAL "")
endif()

# --- Header-only and single-file libraries -----------------------------------
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cli11
  URL ${KUSTAVI_CLI11_URL} URL_HASH SHA256=${KUSTAVI_CLI11_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP ON SYSTEM
  FIND_PACKAGE_ARGS NAMES CLI11)

set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
  URL ${KUSTAVI_SPDLOG_URL} URL_HASH SHA256=${KUSTAVI_SPDLOG_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP ON SYSTEM
  FIND_PACKAGE_ARGS NAMES spdlog)

# stdexec and SQLite are only populated here; their targets are defined below
# because neither one's own build files fit this repo's needs.
FetchContent_Declare(stdexec
  URL ${KUSTAVI_STDEXEC_URL} URL_HASH SHA256=${KUSTAVI_STDEXEC_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR kustavi-no-cmake)
FetchContent_Declare(sqlite3_src
  URL ${KUSTAVI_SQLITE_URL} URL_HASH SHA256=${KUSTAVI_SQLITE_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP ON SOURCE_SUBDIR kustavi-no-cmake)

# The C++ GUI's core library reads JSON and its tests use Catch2.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  URL ${KUSTAVI_NLOHMANN_JSON_URL} URL_HASH SHA256=${KUSTAVI_NLOHMANN_JSON_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP ON SYSTEM)
set(FETCH_NLOHMANN_JSON nlohmann_json)
if(KUSTAVI_BUILD_TESTS)
  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(catch2
    URL ${KUSTAVI_CATCH2_URL} URL_HASH SHA256=${KUSTAVI_CATCH2_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON SYSTEM)
  set(FETCH_CATCH2 catch2)
endif()

FetchContent_MakeAvailable(cli11 spdlog stdexec sqlite3_src ${FETCH_NLOHMANN_JSON} ${FETCH_CATCH2})
if(KUSTAVI_BUILD_TESTS)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

if(NOT TARGET stdexec_kustavi)
  file(GLOB stdexec_scheduler_sources CONFIGURE_DEPENDS
       ${stdexec_SOURCE_DIR}/src/parallel_scheduler/*.cpp)
  add_library(stdexec_kustavi STATIC ${stdexec_scheduler_sources})
  target_include_directories(stdexec_kustavi SYSTEM PUBLIC ${stdexec_SOURCE_DIR}/include)
  target_compile_definitions(stdexec_kustavi PUBLIC
    STDEXEC_HAS_STD_CONCURRENCY=1 STDEXEC_BUILD_PARALLEL_SCHEDULER=1)
  target_compile_features(stdexec_kustavi PUBLIC cxx_std_23)
  find_package(Threads REQUIRED)
  target_link_libraries(stdexec_kustavi PUBLIC Threads::Threads)
endif()

if(NOT TARGET sqlite3_kustavi)
  add_library(sqlite3_kustavi STATIC
    ${sqlite3_src_SOURCE_DIR}/sqlite3.c)
  target_include_directories(sqlite3_kustavi SYSTEM PUBLIC ${sqlite3_src_SOURCE_DIR})
  set_target_properties(sqlite3_kustavi PROPERTIES
    C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
  find_package(Threads REQUIRED)
  target_link_libraries(sqlite3_kustavi PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# --- curl --------------------------------------------------------------------
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
if(WIN32)
  set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
  set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
else()
  set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(curl
  URL ${KUSTAVI_CURL_URL} URL_HASH SHA256=${KUSTAVI_CURL_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP ON SYSTEM
  FIND_PACKAGE_ARGS NAMES CURL)
FetchContent_MakeAvailable(curl)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

# --- OpenCV ------------------------------------------------------------------
# Only the modules the back end links. Codecs come from OpenCV's bundled
# libraries so the result does not depend on what is installed.
set(OPENCV_LIST_MODULES core imgproc imgcodecs objdetect dnn videoio)
set(BUILD_LIST "core,imgproc,imgcodecs,objdetect,dnn,videoio" CACHE STRING "" FORCE)
foreach(opt TESTS PERF_TESTS EXAMPLES DOCS opencv_apps JAVA PACKAGE
            opencv_python3 opencv_java_bindings_generator
            opencv_js_bindings_generator ANDROID_EXAMPLES)
  set(BUILD_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(WITH_IPP OFF CACHE BOOL "" FORCE)
set(WITH_ITT OFF CACHE BOOL "" FORCE)
set(WITH_OPENEXR OFF CACHE BOOL "" FORCE)
set(WITH_OPENCL OFF CACHE BOOL "" FORCE)
set(WITH_FFMPEG OFF CACHE BOOL "" FORCE)
set(WITH_GSTREAMER OFF CACHE BOOL "" FORCE)
# dnn parses ONNX (the YuNet face model) with protobuf. Share the one gRPC
# builds: a second copy of the runtime in the same process would collide.
set(WITH_PROTOBUF ON CACHE BOOL "" FORCE)
set(BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(PROTOBUF_UPDATE_FILES ON CACHE BOOL "" FORCE)
set(Protobuf_DIR ${CMAKE_CURRENT_LIST_DIR}/stubs/Protobuf CACHE PATH "" FORCE)
if(NOT WIN32)
  set(BUILD_ZLIB OFF CACHE BOOL "" FORCE)
endif()
set(OPENCV_FORCE_3RDPARTY_BUILD ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
find_package(Patch REQUIRED)
FetchContent_Declare(opencv
  URL ${KUSTAVI_OPENCV_URL} URL_HASH SHA256=${KUSTAVI_OPENCV_SHA256}
  PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/opencv_subproject.cmake
        COMMAND ${Patch_EXECUTABLE} -p0 -i ${CMAKE_CURRENT_LIST_DIR}/patches/opencv-wstring-convert.patch
  DOWNLOAD_EXTRACT_TIMESTAMP ON SYSTEM
  FIND_PACKAGE_ARGS NAMES OpenCV)
FetchContent_MakeAvailable(opencv)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

# --- llama.cpp ---------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)
set(MTMD_VIDEO OFF CACHE BOOL "" FORCE)
set(GGML_CCACHE OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(APPLE)
  set(GGML_METAL ON CACHE BOOL "" FORCE)
  set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
  set(GGML_ACCELERATE ON CACHE BOOL "" FORCE)
  set(GGML_BLAS ON CACHE BOOL "" FORCE)
  set(GGML_BLAS_VENDOR Apple CACHE STRING "" FORCE)
  set(GGML_NATIVE ON CACHE BOOL "" FORCE)
else()
  set(GGML_METAL OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(GGML_SSE42 ON CACHE BOOL "" FORCE)
  set(GGML_AVX ON CACHE BOOL "" FORCE)
  set(GGML_AVX2 ON CACHE BOOL "" FORCE)
  set(GGML_FMA ON CACHE BOOL "" FORCE)
  set(GGML_F16C ON CACHE BOOL "" FORCE)
  set(GGML_AVX512 OFF CACHE BOOL "" FORCE)
  option(KUSTAVI_LLAMA_VULKAN "Build llama.cpp's Vulkan backend" OFF)
  option(KUSTAVI_LLAMA_CUDA "Build llama.cpp's CUDA backend" OFF)
  set(GGML_VULKAN ${KUSTAVI_LLAMA_VULKAN} CACHE BOOL "" FORCE)
  set(GGML_CUDA ${KUSTAVI_LLAMA_CUDA} CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(llama_cpp
  URL ${KUSTAVI_LLAMA_URL} URL_HASH SHA256=${KUSTAVI_LLAMA_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP ON SYSTEM)
# llama.cpp targets C++17 and is not built with the repo's C++ standard.
set(_kustavi_cxx_std ${CMAKE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD 17)
FetchContent_MakeAvailable(llama_cpp)
set(CMAKE_CXX_STANDARD ${_kustavi_cxx_std})
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

string(REPLACE "|" ";" _kustavi_saved_flags "${_kustavi_saved_flags}")
list(GET _kustavi_saved_flags 0 CMAKE_C_FLAGS)
list(GET _kustavi_saved_flags 1 CMAKE_CXX_FLAGS)
list(GET _kustavi_saved_flags 2 CMAKE_OBJCXX_FLAGS)
