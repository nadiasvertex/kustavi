# Starting a child process, reading its output and watching it exit. The back end's smoke client
# and the GUI both launch the server through it.
add_library(kustavi_process STATIC
  ring_buffer.cpp
  supervised_process.cpp)
if(WIN32)
  target_sources(kustavi_process PRIVATE child_process_windows.cpp)
else()
  target_sources(kustavi_process PRIVATE child_process_posix.cpp)
endif()
add_library(kustavi::process ALIAS kustavi_process)
# Headers are included as "process/<name>.h".
target_include_directories(kustavi_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(kustavi_process PRIVATE kustavi_options)
find_package(Threads REQUIRED)
target_link_libraries(kustavi_process PUBLIC Threads::Threads)

if(KUSTAVI_BUILD_TESTS)
  add_executable(process_fixture process_fixture.cpp)
  add_executable(process_tests
    ring_buffer_test.cpp
    supervised_process_test.cpp)
  target_link_libraries(process_tests PRIVATE kustavi::process kustavi_options
    Catch2::Catch2WithMain)
  target_compile_definitions(process_tests PRIVATE
    PROCESS_FIXTURE="$<TARGET_FILE:process_fixture>")
  add_dependencies(process_tests process_fixture)
  include(Catch)
  catch_discover_tests(process_tests PROPERTIES LABELS process)
endif()
