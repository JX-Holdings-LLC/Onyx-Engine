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
# ---------------------------------------------------------------------------
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   ON  CACHE BOOL "" FORCE)
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)

if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/CMakeLists.txt)
    message(FATAL_ERROR "vendor/llama.cpp is missing - run: git submodule update --init --recursive")
endif()

add_subdirectory(vendor/llama.cpp)

# ---------------------------------------------------------------------------
# jx-engine binary
# ---------------------------------------------------------------------------
add_executable(jx-engine
    src/main.cpp
    src/args.cpp
    src/engine.cpp
    src/server.cpp
)

target_compile_definitions(jx-engine PRIVATE
    JX_ENGINE_VERSION="${PROJECT_VERSION}"
)

target_include_directories(jx-engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/common
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/vendor
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/llama.cpp/vendor/cpp-httplib
)

find_package(Threads REQUIRED)
target_link_libraries(jx-engine PRIVATE llama-common llama Threads::Threads)

install(TARGETS jx-engine RUNTIME DESTINATION bin)
