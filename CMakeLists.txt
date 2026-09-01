cmake_minimum_required(VERSION 3.16)

project(jx-engine
    VERSION 0.1.0
    DESCRIPTION "JX Engine - local model serving binary for JX Runtime"
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
option(JX_ENGINE_CUDA   "Build with CUDA support"   OFF)
option(JX_ENGINE_VULKAN "Build with Vulkan support" OFF)
option(JX_ENGINE_METAL  "Build with Metal support"  OFF)
option(JX_ENGINE_HIP    "Build with ROCm/HIP support" OFF)
option(JX_ENGINE_BLAS   "Build with BLAS support"   OFF)

if (JX_ENGINE_CUDA)
    set(GGML_CUDA ON CACHE BOOL "" FORCE)
endif()
if (JX_ENGINE_VULKAN)
    set(GGML_VULKAN ON CACHE BOOL "" FORCE)
endif()
if (JX_ENGINE_METAL)
    set(GGML_METAL ON CACHE BOOL "" FORCE)
endif()
if (JX_ENGINE_HIP)
    set(GGML_HIP ON CACHE BOOL "" FORCE)
endif()
if (JX_ENGINE_BLAS)
    set(GGML_BLAS ON CACHE BOOL "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Vendored llama.cpp — only the libraries, none of its tools/examples.
#
# The source tree comes from the npm registry (@jxburros/llama-cpp-source,
# installed by `npm ci` into node_modules) so the build's only external input
# is an npm package. Resolution order:
#   1. -DJX_ENGINE_LLAMA_DIR=<path>          explicit override
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
# it shells out to an ffmpeg binary at runtime, and jx-engine takes no
# runtime dependency on external processes.
set(LLAMA_BUILD_MTMD     ON  CACHE BOOL "" FORCE)
set(MTMD_VIDEO           OFF CACHE BOOL "" FORCE)

set(JX_ENGINE_LLAMA_DIR "" CACHE PATH "Path to the llama.cpp source tree (overrides auto-detection)")

if (JX_ENGINE_LLAMA_DIR)
    set(JX_LLAMA_SRC ${JX_ENGINE_LLAMA_DIR})
elseif (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/node_modules/@jxburros/llama-cpp-source/CMakeLists.txt)
    set(JX_LLAMA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/node_modules/@jxburros/llama-cpp-source)
elseif (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/CMakeLists.txt)
    set(JX_LLAMA_SRC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp)
else()
    message(FATAL_ERROR "llama.cpp source tree not found - run: npm ci")
endif()

if (NOT EXISTS ${JX_LLAMA_SRC}/CMakeLists.txt)
    message(FATAL_ERROR "'${JX_LLAMA_SRC}' is not a llama.cpp source tree")
endif()

message(STATUS "jx-engine: using llama.cpp source at ${JX_LLAMA_SRC}")

add_subdirectory(${JX_LLAMA_SRC} ${CMAKE_BINARY_DIR}/llama.cpp)

# ---------------------------------------------------------------------------
# jx-engine binary
# ---------------------------------------------------------------------------
add_executable(jx-engine
    src/main.cpp
    src/args.cpp
    src/convert.cpp
    src/engine.cpp
    src/server.cpp
)

target_compile_definitions(jx-engine PRIVATE
    JX_ENGINE_VERSION="${PROJECT_VERSION}"
)

target_include_directories(jx-engine PRIVATE
    ${JX_LLAMA_SRC}/common
    ${JX_LLAMA_SRC}/tools/mtmd
    ${JX_LLAMA_SRC}/vendor
    ${JX_LLAMA_SRC}/vendor/cpp-httplib
)

find_package(Threads REQUIRED)
target_link_libraries(jx-engine PRIVATE llama-common llama mtmd Threads::Threads)

install(TARGETS jx-engine RUNTIME DESTINATION bin)
