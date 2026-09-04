cmake_minimum_required(VERSION 3.16)

project(onyx-engine
    VERSION 0.2.0
    DESCRIPTION "Onyx Engine - local model serving binary for JX Runtime"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

if (ONYX_ENGINE_CUDA)
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
endif()
if (ONYX_ENGINE_VULKAN)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
endif()
if (ONYX_ENGINE_METAL)
    set(GGML_METAL ON CACHE BOOL "" FORCE)
endif()
if (ONYX_ENGINE_HIP)
    set(GGML_HIP ON CACHE BOOL "" FORCE)
endif()
if (ONYX_ENGINE_BLAS)
    set(GGML_BLAS ON CACHE BOOL "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Vendored llama.cpp — only the libraries, none of its tools/examples.
#
# The source tree comes from the npm registry (@jxburros/llama-cpp-source,
# installed by `npm ci` into node_modules) so the build's only external input
# is an npm package. Resolution order:
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
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)

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
# The npm package records the real pin in its own package.json (written by
# packaging/make-vendor-package.sh), so read it from there. For a git
# checkout (-DONYX_ENGINE_LLAMA_DIR or vendor/llama.cpp) ask git, but only
# after confirming the repository's top level IS that tree - never a parent's.
# ---------------------------------------------------------------------------
set(ONYX_LLAMA_PIN "unknown")

if (EXISTS ${ONYX_LLAMA_SRC}/package.json)
    file(READ ${ONYX_LLAMA_SRC}/package.json ONYX_LLAMA_PKG_JSON)
    # e.g. "version": "0.3.0-b10711.g9723942ad"  ->  b10711-9723942ad
    if (ONYX_LLAMA_PKG_JSON MATCHES "\"version\"[ \t]*:[ \t]*\"[^\"]*-b([0-9]+)\\.g([0-9a-fA-F]+)\"")
        set(ONYX_LLAMA_PIN "b${CMAKE_MATCH_1}-${CMAKE_MATCH_2}")
    endif()
endif()

if (ONYX_LLAMA_PIN STREQUAL "unknown")
    find_package(Git QUIET)
    if (GIT_EXECUTABLE)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --show-toplevel
            WORKING_DIRECTORY ${ONYX_LLAMA_SRC}
            OUTPUT_VARIABLE ONYX_GIT_TOPLEVEL
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE ONYX_GIT_RES
            ERROR_QUIET
        )
        if (ONYX_GIT_RES EQUAL 0)
            get_filename_component(ONYX_GIT_TOPLEVEL "${ONYX_GIT_TOPLEVEL}" REALPATH)
            get_filename_component(ONYX_LLAMA_SRC_REAL "${ONYX_LLAMA_SRC}" REALPATH)
            # only trust git when the repo's root is the llama.cpp tree itself
            if (ONYX_GIT_TOPLEVEL STREQUAL ONYX_LLAMA_SRC_REAL)
                execute_process(COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
                    WORKING_DIRECTORY ${ONYX_LLAMA_SRC}
                    OUTPUT_VARIABLE ONYX_GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE ONYX_GIT_RES_A ERROR_QUIET)
                execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=9 HEAD
                    WORKING_DIRECTORY ${ONYX_LLAMA_SRC}
                    OUTPUT_VARIABLE ONYX_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE ONYX_GIT_RES_B ERROR_QUIET)
                if (ONYX_GIT_RES_A EQUAL 0 AND ONYX_GIT_RES_B EQUAL 0)
                    set(ONYX_LLAMA_PIN "b${ONYX_GIT_COUNT}-${ONYX_GIT_HASH}")
                endif()
            endif()
        endif()
    endif()
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
