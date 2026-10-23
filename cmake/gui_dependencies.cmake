# Basalt and Magma for the C++ GUI. A parent project that declares Basalt first decides which Basalt
# Magma uses, so Basalt is declared here before Magma.
include(FetchContent)

set(KUSTAVI_BASALT_SOURCE_DIR "" CACHE PATH "A Basalt checkout to build the GUI against")
set(KUSTAVI_MAGMA_SOURCE_DIR "" CACHE PATH "A Magma checkout to build the GUI against")

if(NOT KUSTAVI_BASALT_SOURCE_DIR OR NOT KUSTAVI_MAGMA_SOURCE_DIR)
  if(NOT DEFINED KUSTAVI_BASALT_URL OR NOT DEFINED KUSTAVI_MAGMA_URL)
    message(FATAL_ERROR
      "The GUI needs Basalt and Magma, and cmake/dependency_pins.cmake does not pin them yet. "
      "Set KUSTAVI_BASALT_SOURCE_DIR and KUSTAVI_MAGMA_SOURCE_DIR to checkouts.")
  endif()
endif()

set(BASALT_BUILD_TESTS OFF)
set(BASALT_BUILD_TOOLS OFF)
set(BASALT_BUILD_BENCHMARKS OFF)
set(BASALT_INSTALL OFF)
set(MAGMA_BUILD_TESTS OFF)
set(MAGMA_BUILD_BENCHMARKS OFF)
set(MAGMA_BUILD_EXAMPLES OFF)
set(MAGMA_INSTALL OFF)

if(KUSTAVI_BASALT_SOURCE_DIR)
  FetchContent_Declare(basalt SOURCE_DIR "${KUSTAVI_BASALT_SOURCE_DIR}")
else()
  FetchContent_Declare(basalt URL "${KUSTAVI_BASALT_URL}" URL_HASH "SHA256=${KUSTAVI_BASALT_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()
if(KUSTAVI_MAGMA_SOURCE_DIR)
  FetchContent_Declare(magma SOURCE_DIR "${KUSTAVI_MAGMA_SOURCE_DIR}")
else()
  FetchContent_Declare(magma URL "${KUSTAVI_MAGMA_URL}" URL_HASH "SHA256=${KUSTAVI_MAGMA_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
endif()
FetchContent_MakeAvailable(basalt magma)
