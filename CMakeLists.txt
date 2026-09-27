cmake_minimum_required(VERSION 3.16)

project(onyx-engine
    VERSION 0.4
    DESCRIPTION "Onyx Engine - local model serving binary for JX Runtime"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(BUILD_SHARED_LIBS "Build shared dependencies" OFF)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Acceleration options (forwarded to ggml). Exactly one of these is typically
# enabled per build; the CPU backend is always available as a fallback.
# ---------------------------------------------------------------------------
option(ONYX_ENGINE_CUDA   "Build with CUDA support"   OFF)
option(ONYX_ENGINE_VULKAN "Build with Vulkan support" OFF)
option(ONYX_ENGINE_METAL  "Build with Metal support"  OFF)
option(ONYX_ENGINE_HIP    "Build with ROCm/HIP support" OFF)
option(ONYX_ENGINE_BLAS   "Build with BLAS support"   OFF)

set(GGML_CUDA ${ONYX_ENGINE_CUDA} CACHE BOOL "" FORCE)
set(GGML_VULKAN ${ONYX_ENGINE_VULKAN} CACHE BOOL "" FORCE)
set(GGML_METAL ${ONYX_ENGINE_METAL} CACHE BOOL "" FORCE)
set(GGML_HIP ${ONYX_ENGINE_HIP} CACHE BOOL "" FORCE)
set(GGML_BLAS ${ONYX_ENGINE_BLAS} CACHE BOOL "" FORCE)

# ---------------------------------------------------------------------------
# Vendored llama.cpp — only the libraries, none of its tools/examples.
#
# `npm ci` fetches the pinned Git commit into node_modules. Resolution order:
#   1. -DONYX_ENGINE_LLAMA_DIR=<path>          explicit override
#   2. node_modules/@jxburros/llama-cpp-source   (canonical: npm ci)
#   3. vendor/llama.cpp                      manually placed source tree
# ---------------------------------------------------------------------------
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   ON  CACHE BOOL "" FORCE)
set(LLAMA_OPENSSL        OFF CACHE BOOL "" FORCE)

# Multimodal projector support (--mmproj). LLAMA_BUILD_MTMD is upstream's
# escape hatch for building tools/mtmd as a standalone library without the
# rest of the tools/ tree (which stays off above). MTMD_VIDEO is forced off:
# it shells out to an ffmpeg binary at runtime, and onyx-engine takes no
# runtime dependency on external processes.
set(LLAMA_BUILD_MTMD     ON  CACHE BOOL "" FORCE)
set(MTMD_VIDEO           OFF CACHE BOOL "" FORCE)

set(ONYX_ENGINE_LLAMA_DIR "" CACHE PATH "Path to the llama.cpp source tree (overrides auto-detection)")

if (ONYX_ENGINE_LLAMA_DIR)
    set(ONYX_LLAMA_SRC ${ONYX_ENGINE_LLAMA_DIR})
elseif (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/node_modules/@jxburros/llama-cpp-source/CMakeLists.txt)
    set(ONYX_LLAMA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/node_modules/@jxburros/llama-cpp-source)
elseif (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/CMakeLists.txt)
    set(ONYX_LLAMA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp)
else()
    message(FATAL_ERROR "llama.cpp source tree not found - run: npm ci")
endif()

if (NOT EXISTS ${ONYX_LLAMA_SRC}/CMakeLists.txt)
    message(FATAL_ERROR "'${ONYX_LLAMA_SRC}' is not a llama.cpp source tree")
endif()

message(STATUS "onyx-engine: using llama.cpp source at ${ONYX_LLAMA_SRC}")

# ---------------------------------------------------------------------------
# llama.cpp pin string, resolved by us rather than read from llama.cpp's own
# build-info (`llama_build_info()`).
#
# Upstream's cmake/build-info.cmake runs `git rev-parse` with the llama.cpp
# source dir as the working directory. In the canonical build that directory
# is node_modules/@jxburros/llama-cpp-source, which has no .git of its own -
# so git walks UP and reports *onyx-engine's* commit and commit count, which
# then surfaced in `--version` and /props labelled as llama.cpp build info.
#
# Read the source pin from the packaging script, then check the installed
# source stamp or package metadata before compiling it.
# ---------------------------------------------------------------------------
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/packaging/make-vendor-package.sh ONYX_VENDOR_SCRIPT)
if (NOT ONYX_VENDOR_SCRIPT MATCHES "LLAMA_COMMIT=\"([0-9a-f]+)\"")
    message(FATAL_ERROR "cannot read the pinned llama.cpp commit")
endif()
set(ONYX_EXPECTED_LLAMA_COMMIT "${CMAKE_MATCH_1}")
string(LENGTH "${ONYX_EXPECTED_LLAMA_COMMIT}" ONYX_COMMIT_LENGTH)
if (NOT ONYX_COMMIT_LENGTH EQUAL 40)
    message(FATAL_ERROR "llama.cpp commit pin must be 40 hex characters")
endif()
if (NOT ONYX_VENDOR_SCRIPT MATCHES "-b([0-9]+)\\.g")
    message(FATAL_ERROR "cannot read the pinned llama.cpp build number")
endif()
string(SUBSTRING "${ONYX_EXPECTED_LLAMA_COMMIT}" 0 9 ONYX_LLAMA_SHORT)
set(ONYX_LLAMA_PIN "b${CMAKE_MATCH_1}-${ONYX_LLAMA_SHORT}")

if (EXISTS ${ONYX_LLAMA_SRC}/.onyx-source-commit)
    file(READ ${ONYX_LLAMA_SRC}/.onyx-source-commit ONYX_INSTALLED_COMMIT)
elseif (EXISTS ${ONYX_LLAMA_SRC}/package.json)
    file(READ ${ONYX_LLAMA_SRC}/package.json ONYX_LLAMA_PKG_JSON)
    if (ONYX_LLAMA_PKG_JSON MATCHES "\"llamaCppCommit\"[ \t]*:[ \t]*\"([0-9a-f]+)\"")
        set(ONYX_INSTALLED_COMMIT "${CMAKE_MATCH_1}")
    endif()
endif()
if (DEFINED ONYX_INSTALLED_COMMIT)
    string(STRIP "${ONYX_INSTALLED_COMMIT}" ONYX_INSTALLED_COMMIT)
    if (NOT ONYX_INSTALLED_COMMIT STREQUAL ONYX_EXPECTED_LLAMA_COMMIT)
        message(FATAL_ERROR "installed llama.cpp source does not match the pinned commit; run npm ci")
    endif()
elseif (ONYX_LLAMA_SRC STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/node_modules/@jxburros/llama-cpp-source")
    message(FATAL_ERROR "installed llama.cpp source has no verifiable commit stamp; run npm ci")
endif()

message(STATUS "onyx-engine: llama.cpp pin ${ONYX_LLAMA_PIN}")

add_subdirectory(${ONYX_LLAMA_SRC} ${CMAKE_BINARY_DIR}/llama.cpp)

# ---------------------------------------------------------------------------
# cpp-httplib (third_party/cpp-httplib) - onyx-engine's own vendored copy.
#
# The HTTP layer is not llama.cpp's concern. This used to be an include path
# into <llama src>/vendor/cpp-httplib, while the implementation came out of
# libllama-common.so - which links upstream's own cpp-httplib statically and
# re-exports ~1200 httplib::* symbols. That made onyx-engine's server depend on
# a private implementation detail of llama-common, breakable by any upstream
# change to it.
#
# The tuning macros below are PUBLIC here, unlike upstream's PRIVATE, so the
# header and the implementation are compiled with identical values. Upstream's
# choice left src/server.cpp compiling httplib::Server with a different
# tcp_nodelay_ initializer than httplib.cpp used - an ODR mismatch. It was
# latent, not live: Server::Server() is out of line in httplib.cpp, so the
# effective value was upstream's and Nagle was disabled either way.
#
# See third_party/cpp-httplib/README.md.
# ---------------------------------------------------------------------------
add_library(onyx-httplib STATIC third_party/cpp-httplib/httplib.cpp)

target_include_directories(onyx-httplib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib)

target_compile_definitions(onyx-httplib PUBLIC
    CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=1048576
    CPPHTTPLIB_LISTEN_BACKLOG=512
    CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=32768
    CPPHTTPLIB_TCP_NODELAY=1
)

target_compile_features(onyx-httplib PRIVATE cxx_std_17)
set_property(TARGET onyx-httplib PROPERTY POSITION_INDEPENDENT_CODE ON)

# third-party code: don't fail or spam on its warnings
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(onyx-httplib PRIVATE /w)
else()
    target_compile_options(onyx-httplib PRIVATE -w)
endif()

find_package(Threads REQUIRED)
target_link_libraries(onyx-httplib PRIVATE Threads::Threads)
if (WIN32 AND NOT MSVC)
    target_link_libraries(onyx-httplib PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# onyx-engine binary
# ---------------------------------------------------------------------------
add_executable(onyx-engine
    src/main.cpp
    src/args.cpp
    src/convert.cpp
    src/engine.cpp
    src/server.cpp
)

target_compile_definitions(onyx-engine PRIVATE
    ONYX_ENGINE_VERSION="${PROJECT_VERSION}"
    ONYX_ENGINE_LLAMA_PIN="${ONYX_LLAMA_PIN}"
    $<$<CONFIG:Debug>:ONYX_ENGINE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}">
)

target_include_directories(onyx-engine PRIVATE
    ${ONYX_LLAMA_SRC}/common
    ${ONYX_LLAMA_SRC}/tools/mtmd
)

# onyx-httplib comes FIRST on purpose. llama-common is built as a shared library
# with upstream's own cpp-httplib linked into it, and it re-exports ~1200
# httplib symbols; if it is processed first, part of onyx-engine's HTTP layer
# binds to llama.cpp's copy at runtime instead of ours. Our archive first
# means every httplib symbol is resolved from third_party/cpp-httplib.
target_link_libraries(onyx-engine PRIVATE onyx-httplib llama-common llama mtmd Threads::Threads)

install(TARGETS onyx-engine RUNTIME DESTINATION bin)
