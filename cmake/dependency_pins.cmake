# Every third-party dependency's source location and checksum. Bump a version
# and its hash together. gRPC is pinned by git commit because its release
# tarballs omit the submodules that its own CMake build needs; the commit hash
# is as immutable as a SHA-256.

set(KUSTAVI_GRPC_GIT_URL "https://github.com/grpc/grpc.git")
set(KUSTAVI_GRPC_GIT_TAG "3252a89f10d8e92997862167ca7d095ecda85973") # v1.84.0

set(KUSTAVI_OPENCV_URL "https://github.com/opencv/opencv/archive/refs/tags/5.0.0.tar.gz")
set(KUSTAVI_OPENCV_SHA256 "b0528f5a1d379d59d4701cb28c36e22214cc51cf64594e5b56f2d3e6c0233095")

set(KUSTAVI_CURL_URL "https://github.com/curl/curl/releases/download/curl-8_21_0/curl-8.21.0.tar.xz")
set(KUSTAVI_CURL_SHA256 "aa1b66a70eace83dc624508745646c08ae561de512ab403adffb93ac87fc72e6")

set(KUSTAVI_SPDLOG_URL "https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz")
set(KUSTAVI_SPDLOG_SHA256 "d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744")

set(KUSTAVI_CLI11_URL "https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.6.2.tar.gz")
set(KUSTAVI_CLI11_SHA256 "c6ea6b2e5608b3ea8617999bd5f47420c71b2ebdb8dc4767c1034d1da5785711")

set(KUSTAVI_SQLITE_URL "https://sqlite.org/2026/sqlite-amalgamation-3530300.zip")
set(KUSTAVI_SQLITE_SHA256 "646421e12aac110282ef8cc68f1a62d4bb15fc7b8f09da0b53e29ee690500431")

set(KUSTAVI_STDEXEC_URL "https://github.com/NVIDIA/stdexec/archive/e92245d53dfc37f9bc2a7ef0cc44d570bb7b3386.tar.gz")
set(KUSTAVI_STDEXEC_SHA256 "534099765fcd7ae16470f5d934006639fecd0f967473eccc01d7d0c1a314ee07")

set(KUSTAVI_LLAMA_URL "https://github.com/ggml-org/llama.cpp/archive/refs/tags/b10733.tar.gz")
set(KUSTAVI_LLAMA_SHA256 "07de784d07b346647da6bd8796a42127304c7508dd007a21f63ab74da21a13cf")

# Basalt (widgets) and Magma (windows, input and rendering) are not pinned yet. The GUI needs
# Basalt's slider, grid_view and image_tile and Magma's folder picker, and the releases that carry
# them do not exist. When they do, add the release archive and SHA-256 of each here, and
# cmake/gui_dependencies.cmake will use them. Until then, configure with
# KUSTAVI_BASALT_SOURCE_DIR and KUSTAVI_MAGMA_SOURCE_DIR.

# The C++ GUI's core library and its tests.
set(KUSTAVI_NLOHMANN_JSON_URL "https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz")
set(KUSTAVI_NLOHMANN_JSON_SHA256 "42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa")

set(KUSTAVI_CATCH2_URL "https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz")
set(KUSTAVI_CATCH2_SHA256 "0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34")
