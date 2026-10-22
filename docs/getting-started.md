[Documentation](README.md) › Getting started

# Getting started

From nothing to a streamed chat completion. You need a model; everything else is below.

## 1. Get the binary

**Prebuilt.** Each release attaches an archive per platform to [GitHub Releases](https://github.com/JX-Holdings-LLC/Onyx-Engine/releases), with a `checksums.txt` beside them:

| Platform | Archive |
|---|---|
| Linux, x64 | `onyx-engine-v<version>-linux-x64.tar.gz` |
| macOS, Apple Silicon | `onyx-engine-v<version>-darwin-arm64.tar.gz` |
| macOS, Intel | `onyx-engine-v<version>-darwin-x64.tar.gz` |
| Windows, x64 | `onyx-engine-v<version>-win32-x64.zip` |

```bash
sha256sum -c --ignore-missing checksums.txt
tar -xzf onyx-engine-v<version>-linux-x64.tar.gz
./onyx-engine --version
```

The archive holds the binary and three licence files, flat. The prebuilt binaries are CPU-only, and the x64 builds require a CPU with AVX2. For a GPU, build from source.

**From source.** You need CMake 3.16 or later (3.21 for the presets), a C++17 compiler, a C compiler and Git. Node.js is not needed.

```bash
git clone https://github.com/JX-Holdings-LLC/Onyx-Engine.git
cd Onyx-Engine
cmake -B build -DCMAKE_BUILD_TYPE=Release     # fetches and verifies the pinned llama.cpp commit with Git
cmake --build build --target onyx-engine -j 8
./build/onyx-engine --version
```

Or with a preset: `cmake --preset release && cmake --build --preset release` (the binary lands in `build/release/`). If you have Node.js, `npm ci && npm run build` does the same job and produces the same binary at `build/onyx-engine`.

A cold build compiles all of llama.cpp and takes several minutes. For CUDA, Vulkan, Metal, ROCm or BLAS, see [building](development/building.md#acceleration-option-matrix).

## 2. Start it on a model

Any GGUF will do:

```bash
./build/onyx-engine -m ./models/your-model.gguf --alias my-model --port 8080
```

On success the process prints four lines on stderr and then sits and serves:

```text
onyx-engine listening on http://127.0.0.1:8080 (loading)
onyx-engine 0.6.0: loading model './models/your-model.gguf'
onyx-engine: model loaded (<description>, <size> MiB)
onyx-engine ready on http://127.0.0.1:8080 (model: my-model, ctx: 4096)
```

It binds `127.0.0.1` only. The port opens at once: `GET /health` answers `503` while the model loads and `200` once it is ready. Pass `--port 0` to let the system choose a free port; the `listening on` line says which. A safetensors directory works too (`-m ./Llama-3-8B-hf/`); it is converted to GGUF once and cached. See [serving models](guides/serving-models.md).

## 3. Ask it something

```bash
curl http://127.0.0.1:8080/health
# {"status":"ok"}

curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello!"}]}'
```

Stream it by adding `"stream": true` and telling curl not to buffer:

```bash
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Count to five."}],"stream":true}'
```

You get `data: {...}` frames, then a final frame with `usage` and `timings` and an empty `choices`, then `data: [DONE]`. Any OpenAI-compatible client works if you point its base URL at `http://127.0.0.1:8080/v1`.

## 4. See what you are talking to

```bash
curl http://127.0.0.1:8080/props          # model, chat template, per-slot n_ctx, modalities
curl http://127.0.0.1:8080/v1/models      # exactly one entry: your alias
curl -d '{"content":"Hello!"}' http://127.0.0.1:8080/tokenize
```

## 5. Run the tests, offline

```bash
bash scripts/smoke-test.sh    # or: npm test
```

The smoke test generates its own random-weight models with a small numpy script, so it needs `python3` with numpy, `curl` and Bash, and downloads nothing. It checks the shape of every response, not the words, because the weights are random. See [testing and CI](development/testing-and-ci.md).

## Where next

| You want to | Read |
|---|---|
| Size context, slots and memory sensibly | [Serving models](guides/serving-models.md) |
| Get JSON or tool calls out reliably | [Structured output and tools](guides/structured-output-and-tools.md) |
| Send images or audio | [Multimodal input](guides/multimodal.md) |
| Serve embeddings or rerank | [Embeddings and reranking](guides/embeddings-and-reranking.md) |
| Put it behind a key, or on a network | [Security](guides/security.md) |
| Work out why something answered 400 or 501 | [Troubleshooting](guides/troubleshooting.md) |
| Drive it from JX Runtime | [JX Runtime integration](integration/jx-runtime.md) |
