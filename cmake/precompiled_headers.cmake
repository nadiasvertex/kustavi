# Precompiled headers for Kustavi's larger targets. Each target precompiles the standard headers
# that most of the sources include, plus the third-party headers it names. The generated protobuf
# header is the costliest to parse, so every target that reaches the engine's API names it.
#
# A source still includes what it uses: configure with -DKUSTAVI_PCH=OFF to check that it builds
# without them.

option(KUSTAVI_PCH "Precompile common headers for Kustavi's larger targets" ON)

set(KUSTAVI_PCH_STD_HEADERS
  <algorithm> <array> <atomic> <chrono> <condition_variable> <cstdint> <filesystem> <format>
  <functional> <map> <memory> <mutex> <optional> <string> <string_view> <thread>
  <unordered_map> <unordered_set> <utility> <vector>)

# kustavi_precompile_headers(<target> [<header>...])
function(kustavi_precompile_headers target)
  if(NOT KUSTAVI_PCH)
    return()
  endif()
  target_precompile_headers(${target} PRIVATE ${KUSTAVI_PCH_STD_HEADERS} ${ARGN})
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang stamps a precompiled header with the time it was built, which ccache cannot cache.
    target_compile_options(${target} PRIVATE -Xclang -fno-pch-timestamp)
  endif()
endfunction()
