set(BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# All back-end binaries share one output directory so the smoke client finds
# the server beside itself and `data/` is found next to every executable.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set(OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs opencv_objdetect
                opencv_dnn opencv_videoio)

# Include directories OpenCV's own targets do not export when it is built as
# a subproject.
function(kustavi_opencv_includes target)
  set(dirs ${opencv_SOURCE_DIR}/include ${CMAKE_BINARY_DIR})
  foreach(m core imgproc imgcodecs objdetect dnn videoio)
    list(APPEND dirs ${opencv_SOURCE_DIR}/modules/${m}/include)
  endforeach()
  target_include_directories(${target} SYSTEM PRIVATE ${dirs})
endfunction()

function(kustavi_executable name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;OPENCV" ${ARGN})
  add_executable(${name} ${ARG_SOURCES})
  target_include_directories(${name} PRIVATE ${BACKEND_DIR}/include)
  target_link_libraries(${name} PRIVATE kustavi_options ${ARG_LIBS})
  if(ARG_OPENCV)
    kustavi_opencv_includes(${name})
  endif()
  if(MSVC)
    target_link_options(${name} PRIVATE oleaut32.lib)
  endif()
endfunction()

# --- Server ------------------------------------------------------------------
file(GLOB_RECURSE SERVER_SOURCES CONFIGURE_DEPENDS src/*.cpp)

kustavi_executable(server
  SOURCES ${SERVER_SOURCES}
  OPENCV ON
  LIBS kustavi::proto gRPC::grpc++ CURL::libcurl ${OPENCV_LIBS}
       llama mtmd sqlite3_kustavi stdexec_kustavi CLI11::CLI11
       spdlog::spdlog)

# Data files the server looks up beside the executable (backend/include/paths.h).
add_custom_command(TARGET server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:server>/data
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${BACKEND_DIR}/data/cities.tsv
          ${BACKEND_DIR}/data/face_detection_yunet.onnx
          $<TARGET_FILE_DIR:server>/data
  VERBATIM)

if(NOT KUSTAVI_BUILD_TESTS)
  return()
endif()

# --- Tests -------------------------------------------------------------------
# Each test compiles only the sources it exercises. Data files are copied by
# the server target, so every test depends on it having been built.
function(kustavi_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;OPENCV;LABELS;ENV" ${ARGN})
  kustavi_executable(${name} SOURCES ${ARG_SOURCES} LIBS ${ARG_LIBS}
                     OPENCV ${ARG_OPENCV})
  add_dependencies(${name} server)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABELS}")
  if(ARG_ENV)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${ARG_ENV}")
  endif()
endfunction()

kustavi_test(trips_test
  SOURCES src/geo/place_index.cpp src/pass/trips.cpp test/trips_test.cpp
  LABELS backend
  ENV "KUSTAVI_GEO_DATA=${BACKEND_DIR}/data/cities.tsv")

kustavi_test(repair_test
  SOURCES src/pass/repair.cpp test/repair_test.cpp
  LABELS backend)

kustavi_test(related_files_test
  SOURCES src/pass/commit.cpp src/pass/related_files.cpp
          test/related_files_test.cpp
  LIBS sqlite3_kustavi spdlog::spdlog
  LABELS backend)

kustavi_test(keeper_signals_test
  SOURCES src/pass/keeper_signals.cpp test/keeper_signals_test.cpp
  LIBS ${OPENCV_LIBS} spdlog::spdlog
  OPENCV ON
  LABELS backend)

kustavi_test(database_test
  SOURCES src/store/database.cpp test/database_test.cpp
  LIBS sqlite3_kustavi
  LABELS backend)

kustavi_test(quality_test
  SOURCES src/collection/vantage_point_tree.cpp src/pass/quality.cpp
          src/pass/similar.cpp test/quality_test.cpp
  LIBS ${OPENCV_LIBS} spdlog::spdlog stdexec_kustavi
  OPENCV ON
  LABELS quality
  ENV "KUSTAVI_TEST_PHOTOS=${PROJECT_SOURCE_DIR}/test/photos")

kustavi_test(video_test
  SOURCES src/pass/junk.cpp src/pass/quality.cpp src/pass/similar.cpp
          src/pass/video.cpp test/video_test.cpp
  LIBS llama mtmd ${OPENCV_LIBS} spdlog::spdlog stdexec_kustavi
  OPENCV ON
  LABELS video
  ENV "KUSTAVI_TEST_VIDEOS=${PROJECT_SOURCE_DIR}/test/videos")

# Links llama.cpp and prints the compute devices it found. Not part of the
# default test set; run with `ctest -L llama`.
kustavi_test(llama_smoke
  SOURCES test/llama_smoke.cpp
  LIBS llama
  LABELS llama)

# The smoke client spawns the server as a child process. It uses POSIX process
# APIs and is not built on Windows.
if(NOT WIN32)
  kustavi_executable(smoke_client
    SOURCES test/smoke_client.cpp
    LIBS kustavi::proto gRPC::grpc++)
  add_dependencies(smoke_client server)
  # Runs against a scratch copy of test/photos (see cmake/run_smoke.cmake).
  add_test(NAME smoke
    COMMAND ${CMAKE_COMMAND}
            -DSMOKE_CLIENT=$<TARGET_FILE:smoke_client>
            -DSERVER=$<TARGET_FILE:server>
            -DPHOTOS=${PROJECT_SOURCE_DIR}/test/photos
            "-DSMOKE_ARGS=--concurrency-check;--cancel-check;--resume-check;--batch-check"
            -P ${PROJECT_SOURCE_DIR}/cmake/run_smoke.cmake)
  set_tests_properties(smoke PROPERTIES LABELS backend TIMEOUT 600)

  add_test(NAME smoke_junk
    COMMAND ${CMAKE_COMMAND}
            -DSMOKE_CLIENT=$<TARGET_FILE:smoke_client>
            -DSERVER=$<TARGET_FILE:server>
            -DPHOTOS=${PROJECT_SOURCE_DIR}/test/photos
            "-DSMOKE_ARGS=--junk-check"
            -DNO_DESTINATION=ON
            -P ${PROJECT_SOURCE_DIR}/cmake/run_smoke.cmake)
  # Downloads ~3.3 GB of model weights on first run.
  set_tests_properties(smoke_junk PROPERTIES LABELS junk TIMEOUT 3600)
endif()
