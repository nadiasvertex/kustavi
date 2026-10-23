# Runs the smoke client against a scratch copy of the test photos.
#
# Inputs: SMOKE_CLIENT, SERVER, PHOTOS, SMOKE_ARGS (list), and optionally
# NO_DESTINATION to omit --destination.
string(RANDOM LENGTH 12 suffix)
set(scratch "$ENV{TMPDIR}")
if(NOT scratch)
  set(scratch "/tmp")
endif()
set(scratch "${scratch}/kustavi-smoke-${suffix}")
file(MAKE_DIRECTORY ${scratch})
file(COPY ${PHOTOS} DESTINATION ${scratch})

set(args --server ${SERVER} --folder ${scratch}/photos)
if(NOT NO_DESTINATION)
  list(APPEND args --destination ${scratch}/committed)
endif()
list(APPEND args ${SMOKE_ARGS})

execute_process(COMMAND ${SMOKE_CLIENT} ${args} RESULT_VARIABLE result)
file(REMOVE_RECURSE ${scratch})
if(NOT result EQUAL 0)
  message(FATAL_ERROR "smoke client failed (${result})")
endif()
