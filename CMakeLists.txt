cmake_minimum_required(VERSION 3.28)

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt KUSTAVI_VERSION LIMIT_COUNT 1)
project(kustavi VERSION ${KUSTAVI_VERSION} LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(KUSTAVI_SANITIZE "Build Kustavi's own targets with ASan and UBSan" OFF)
option(KUSTAVI_BUILD_TESTS "Build and register the back-end tests" ON)
option(KUSTAVI_BUILD_GUI "Build the C++ GUI, which needs Basalt and Magma" OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

if(MSVC)
  # UTF-8 sources, a conforming __cplusplus, and the conforming preprocessor
  # (stdexec and {fmt} both require them).
  add_compile_options(/utf-8 /Zc:__cplusplus /Zc:preprocessor)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# gRPC's filter-fusion templates do not compile with the MSVC preview
# toolchain. The macro turns RegisterFusedFilters() into a no-op; it appears in
# no header, so setting it globally is ODR-safe.
if(MSVC)
  add_compile_definitions(GRPC_NO_FILTER_FUSION)
endif()

include(dependencies)

# Warning and sanitizer flags apply only to Kustavi's own targets.
add_library(kustavi_options INTERFACE)
if(MSVC)
  target_compile_options(kustavi_options INTERFACE /W4)
else()
  target_compile_options(kustavi_options INTERFACE -Wall -Wextra)
  if(KUSTAVI_SANITIZE)
    # The dependencies are built without the sanitizers, and they share containers with Kustavi's
    # code. libc++'s container annotations would then flag memory that the unsanitized side wrote.
    target_compile_options(kustavi_options INTERFACE
      -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_compile_definitions(kustavi_options INTERFACE __SANITIZER_DISABLE_CONTAINER_OVERFLOW__)
    target_link_options(kustavi_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

if(KUSTAVI_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(common/process)
add_subdirectory(proto)
add_subdirectory(backend)

if(KUSTAVI_BUILD_GUI)
  include(gui_dependencies)
endif()
add_subdirectory(gui)
