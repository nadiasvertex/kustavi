# Basalt and Magma for the C++ GUI. A parent project that declares Basalt first decides which Basalt
# Magma uses, so Basalt is declared here before Magma.
include(FetchContent)

set(KUSTAVI_BASALT_SOURCE_DIR "" CACHE PATH "A Basalt checkout to build the GUI against")
set(KUSTAVI_MAGMA_SOURCE_DIR "" CACHE PATH "A Magma checkout to build the GUI against")

if(NOT KUSTAVI_BASALT_SOURCE_DIR AND NOT DEFINED KUSTAVI_BASALT_URL)
  message(FATAL_ERROR "cmake/dependency_pins.cmake does not pin Basalt. "
    "Set KUSTAVI_BASALT_SOURCE_DIR to a checkout.")
endif()
if(NOT KUSTAVI_MAGMA_SOURCE_DIR AND NOT DEFINED KUSTAVI_MAGMA_URL)
  message(FATAL_ERROR "cmake/dependency_pins.cmake does not pin Magma. "
    "Set KUSTAVI_MAGMA_SOURCE_DIR to a checkout.")
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

# libmdk for video playback. KUSTAVI_MDK_DIR points at an mdk-sdk to use instead of the pinned
# archive, and KUSTAVI_ENABLE_VIDEO=OFF builds without video, so the detail view shows the still
# image of a video.
option(KUSTAVI_ENABLE_VIDEO "Play videos in the GUI with libmdk" ON)
set(KUSTAVI_MDK_DIR "" CACHE PATH "An mdk-sdk to use instead of the pinned archive")
if(KUSTAVI_ENABLE_VIDEO AND NOT KUSTAVI_MDK_DIR AND DEFINED KUSTAVI_MDK_URL)
  FetchContent_Declare(mdk URL "${KUSTAVI_MDK_URL}" URL_HASH "SHA256=${KUSTAVI_MDK_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
  FetchContent_MakeAvailable(mdk)
  set(KUSTAVI_MDK_DIR "${mdk_SOURCE_DIR}")
endif()
