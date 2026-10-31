# syntax=docker/dockerfile:1
#
# Onyx Engine container images. Built from source with CMake alone (the
# first configure fetches the pinned llama.cpp commit with Git), statically
# linked, no Node.
#
#   docker build -t onyx-engine .                     # CPU (default target)
#   docker build -t onyx-engine:cuda --target cuda .  # NVIDIA CUDA
#
#   docker run --rm -p 127.0.0.1:8080:8080 -v "$PWD/models:/models:ro" \
#       onyx-engine -m /models/model.gguf --allow-unauthenticated
#
# The entrypoint binds 0.0.0.0:8080 inside the container, which onyx-engine
# refuses without --api-key unless --allow-unauthenticated is passed. Publish
# the port on 127.0.0.1 (as above) or pass --api-key. See
# docs/guides/docker.md.

ARG UBUNTU_VERSION=24.04
ARG CUDA_VERSION=12.6.3

# ---------------------------------------------------------------- CPU build
FROM ubuntu:${UBUNTU_VERSION} AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# x86-64 builds assume AVX2, like the release archives; set ONYX_X64_FLAGS=""
# for a baseline build that runs on any x86-64 CPU.
ARG ONYX_X64_FLAGS="-DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON"
RUN cpu_flags=""; [ "$(uname -m)" = x86_64 ] && cpu_flags="$ONYX_X64_FLAGS"; \
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
          -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF $cpu_flags \
 && cmake --build build --target onyx-engine -j"$(nproc)" \
 && build/onyx-engine --version

# ------------------------------------------------------------- CPU runtime
FROM ubuntu:${UBUNTU_VERSION} AS cpu
COPY --from=build /src/build/onyx-engine /usr/local/bin/onyx-engine
COPY --from=build /src/LICENSE /usr/share/doc/onyx-engine/LICENSE
COPY --from=build /src/third_party/cpp-httplib/LICENSE /usr/share/doc/onyx-engine/LICENSE-cpp-httplib
COPY --from=build /src/third_party/nlohmann/LICENSE.MIT /usr/share/doc/onyx-engine/LICENSE-nlohmann-json
COPY --from=build /src/.deps/llama.cpp/LICENSE /usr/share/doc/onyx-engine/LICENSE-llama.cpp
RUN useradd --system --uid 10001 --no-create-home onyx
USER onyx
EXPOSE 8080
ENTRYPOINT ["onyx-engine", "--host", "0.0.0.0", "--port", "8080"]

# --------------------------------------------------------------- CUDA build
FROM nvidia/cuda:${CUDA_VERSION}-devel-ubuntu${UBUNTU_VERSION} AS build-cuda
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake git ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# Turing through Hopper; override for other GPUs.
ARG CUDA_ARCHITECTURES="75;80;86;89;90"
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
          -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DONYX_ENGINE_CUDA=ON \
          -DCMAKE_CUDA_ARCHITECTURES="$CUDA_ARCHITECTURES" \
          -DCMAKE_EXE_LINKER_FLAGS="-Wl,--allow-shlib-undefined" \
 && cmake --build build --target onyx-engine -j"$(nproc)"

# ------------------------------------------------------------ CUDA runtime
FROM nvidia/cuda:${CUDA_VERSION}-runtime-ubuntu${UBUNTU_VERSION} AS cuda
COPY --from=build-cuda /src/build/onyx-engine /usr/local/bin/onyx-engine
COPY --from=build-cuda /src/LICENSE /usr/share/doc/onyx-engine/LICENSE
COPY --from=build-cuda /src/third_party/cpp-httplib/LICENSE /usr/share/doc/onyx-engine/LICENSE-cpp-httplib
COPY --from=build-cuda /src/third_party/nlohmann/LICENSE.MIT /usr/share/doc/onyx-engine/LICENSE-nlohmann-json
COPY --from=build-cuda /src/.deps/llama.cpp/LICENSE /usr/share/doc/onyx-engine/LICENSE-llama.cpp
RUN useradd --system --uid 10001 --no-create-home onyx
USER onyx
EXPOSE 8080
ENTRYPOINT ["onyx-engine", "--host", "0.0.0.0", "--port", "8080"]

# The default target is the CPU image.
FROM cpu
