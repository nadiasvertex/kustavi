set(BACKEND_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# The back-end tests share one output directory.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

set(OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs opencv_objdetect
                opencv_dnn opencv_videoio)

# Include directories OpenCV's own targets do not export when it is built as
# a subproject.
function(kustavi_opencv_includes target)
  set(scope PRIVATE)
  if(ARGC GREATER 1)
    set(scope ${ARGV1})
  endif()
  set(dirs ${opencv_SOURCE_DIR}/include ${CMAKE_BINARY_DIR})
  foreach(m core imgproc imgcodecs objdetect dnn videoio)
    list(APPEND dirs ${opencv_SOURCE_DIR}/modules/${m}/include)
  endforeach()
  target_include_directories(${target} SYSTEM ${scope} ${dirs})
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

# --- Engine ------------------------------------------------------------------
# Everything that the back end does, as a library. The GUI links it and calls it directly.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(kustavi_engine STATIC ${ENGINE_SOURCES})
target_include_directories(kustavi_engine PUBLIC ${BACKEND_DIR}/include)
target_link_libraries(kustavi_engine
  PUBLIC kustavi::proto sqlite3_kustavi stdexec_kustavi
  PRIVATE kustavi_options CURL::libcurl ${OPENCV_LIBS} llama mtmd spdlog::spdlog)
# The engine's headers use OpenCV types and sqlite3.h (through store/database.h) and stdexec. macOS
# finds the system sqlite3.h anyway, which hid the missing include path there.
kustavi_opencv_includes(kustavi_engine PUBLIC)
kustavi_precompile_headers(kustavi_engine
  <proto/service.pb.h> <spdlog/spdlog.h> <opencv2/core.hpp> <opencv2/imgproc.hpp>)
add_library(kustavi::engine ALIAS kustavi_engine)

# Data files that the engine looks up beside the executable (backend/include/paths.h).
# A program that links the engine calls this to have them beside it in the build tree.
function(kustavi_copy_engine_data target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${target}>/data
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PROJECT_SOURCE_DIR}/backend/data/cities.tsv
            ${PROJECT_SOURCE_DIR}/backend/data/face_detection_yunet.onnx
            $<TARGET_FILE_DIR:${target}>/data
    VERBATIM)
endfunction()

if(NOT KUSTAVI_BUILD_TESTS)
  return()
endif()

# --- Tests -------------------------------------------------------------------
# Each unit test compiles only the sources it exercises. The engine tests link the library.
function(kustavi_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS;OPENCV;LABELS;ENV" ${ARGN})
  kustavi_executable(${name} SOURCES ${ARG_SOURCES} LIBS ${ARG_LIBS}
                     OPENCV ${ARG_OPENCV})
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


# The engine against a copy of test/photos. Its junk test downloads the vision model, so that one
# is hidden from the default run and has its own label.
add_executable(engine_test test/engine_test.cpp)
target_link_libraries(engine_test PRIVATE kustavi_options kustavi::engine Catch2::Catch2WithMain)
target_compile_definitions(engine_test PRIVATE KUSTAVI_TEST_PHOTOS="${PROJECT_SOURCE_DIR}/test/photos")
kustavi_copy_engine_data(engine_test)
include(Catch)
catch_discover_tests(engine_test TEST_SPEC "~[junk]" PROPERTIES LABELS backend TIMEOUT 600)
add_test(NAME engine_junk COMMAND engine_test "[junk]")
set_tests_properties(engine_junk PROPERTIES LABELS junk TIMEOUT 3600)
