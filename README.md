# JX Engine

JX Engine is a single-model, OpenAI-compatible model-serving binary built on
[llama.cpp](https://github.com/ggml-org/llama.cpp) (vendored as a pinned
source tree, currently `v0.3.0-90-g9723942ad`, distributed as the npm package
`@jxburros/llama-cpp-source`). One `jx-engine` process loads one GGUF
model and serves it over HTTP on `127.0.0.1:<port>`.

## Relationship to JX Runtime

JX Engine exists so [JX Runtime](https://github.com/jxburros/JX-Runtime) — a
Node.js model runtime — can spawn `jx-engine` in place of a third-party
`llama-server` binary: one `jx-engine` process per loaded model, polled at
`GET /health`, with its OpenAI-compatible endpoints proxied through to
callers. See [`docs/jx-runtime-integration.md`](docs/jx-runtime-integration.md)
for exactly how the two projects are meant to connect; that mapping is a
description of the intended contract, not a claim that JX Runtime has an
adapter for JX Engine today (it does not — see that document's last section).

## Features (v1)

- OpenAI-compatible `/v1/chat/completions`, `/v1/completions`, and
  `/v1/embeddings`, plus `/v1/models`
- llama-server-style utility endpoints: `/health`, `/props`, `/tokenize`,
  `/detokenize`, `/apply-template`
- SSE streaming for chat and text completions, ending in a final frame that
  carries `usage` and `timings` with empty `choices`, then `data: [DONE]`
- Jinja chat templates (via llama.cpp's `common` library), with tool-call
  parsing and streamed tool-call deltas in chat completions
- Structured output via GBNF grammar, `json_schema`, or `response_format`
- Embeddings via `--embedding` mode (pooled, L2-normalized)
- KV-cache common-prefix reuse (`--cache-reuse`) to avoid re-processing an
  unchanged prompt prefix
- CPU by default; optional CUDA, Vulkan, Metal, ROCm/HIP, or BLAS
  acceleration selected at build time via CMake options
- Optional bearer-token auth (`--api-key`) on every endpoint except `/health`

**v1 scope and limits**, stated up front rather than discovered later:

- One model per process. Running a second model means starting a second
  `jx-engine` process on a different port.
- Requests are serialized: one generation runs at a time per process.
  `--parallel`/`-np` is accepted for command-line compatibility with
  `llama-server`, but extra "slots" are not implemented — requests beyond
  the first simply queue behind the mutex in `jx_engine::generate`.
- GGUF only. No safetensors.

See [Roadmap](#roadmap) for what is explicitly out of scope for v1.

## Quick Start

```bash
git clone <this-repo>
cd JX-Engine
npm ci   # or: npm install

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jx-engine -j

./build/jx-engine -m model.gguf --port 8080
```

(`npm run build` does the two `cmake` steps above in one command; `npm test`
runs `scripts/smoke-test.sh`.)

Then, in another terminal:

```bash
curl http://127.0.0.1:8080/health

curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello!"}]}'
```

> **`npm ci` doesn't work yet.** `@jxburros/llama-cpp-source` has not been
> published to the npm registry, so `npm ci`/`npm install` currently fails.
> Until it's published, build the package locally and install it from the
> tarball — see [Vendored source via npm](#vendored-source-via-npm) below and
> [`docs/building.md`](docs/building.md).

For acceleration builds (CUDA, Vulkan, Metal, ROCm, BLAS) see
[`docs/building.md`](docs/building.md).

## Vendored source via npm

JX Engine's only external build input is llama.cpp's C/C++ source tree,
distributed as the npm package `@jxburros/llama-cpp-source` — a pinned,
pruned copy of llama.cpp (not a Node.js library). Using the npm registry as
that channel, rather than a git submodule, means the build has exactly one
trusted external source to fetch from and verify, instead of two (git +
npm). `npm ci` installs it into `node_modules`, and `CMakeLists.txt` picks it
up from there automatically. See
["What llama.cpp's own build is told to skip"](docs/building.md#what-llamacpps-own-build-is-told-to-skip)
and the [architecture doc](docs/architecture.md#build-layering) for the full
source-resolution order (npm package → `-DJX_ENGINE_LLAMA_DIR` override →
manually placed `vendor/llama.cpp`).

**Status: not yet published.** `@jxburros/llama-cpp-source` is not on the
npm registry today, so a fresh clone's `npm ci` fails until the maintainer
runs `packaging/make-vendor-package.sh` and `npm publish <tarball> --access
public`. Until then, build the tarball yourself and install it locally:

```bash
packaging/make-vendor-package.sh
npm install ./packaging/dist/jxburros-llama-cpp-source-*.tgz --no-save
```

See [`docs/building.md`](docs/building.md#packaging--publishing-the-vendor-source-package)
for the full packaging/publishing workflow.

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/health` | Liveness check; exempt from `--api-key` auth |
| `GET` | `/props` | Model/build metadata (alias, chat template, context size, etc.) |
| `GET` | `/v1/models` | OpenAI-style model list (always exactly one model) |
| `POST` | `/tokenize` | Text → token ids |
| `POST` | `/detokenize` | Token ids → text |
| `POST` | `/apply-template` | Render a chat prompt from `messages` without generating |
| `POST` | `/v1/chat/completions` | Chat completion, streaming or not |
| `POST` | `/v1/completions` | Text completion, streaming or not |
| `POST` | `/v1/embeddings` | Pooled embeddings (only when started with `--embedding`) |

Full request/response detail, including exactly which JSON fields are read
and the SSE frame format, is in [`docs/api.md`](docs/api.md).

## CLI flags

`jx-engine --help` is the source of truth; the flags below mirror
`src/args.h`/`src/args.cpp`.

| Flag | Default | Meaning |
|---|---|---|
| `-m, --model PATH` | (required) | GGUF model file |
| `-a, --alias NAME` | file stem | model id reported by the API |
| `--chat-template NAME` | — | override with a built-in template |
| `--chat-template-file F` | — | override with a template read from a file |
| `--jinja` | on | accepted for compatibility; jinja is always used |
| `--host HOST` | `127.0.0.1` | bind address |
| `--port PORT` | `8080` | listen port |
| `--api-key KEY` | — | require this bearer token on every endpoint but `/health` |
| `-c, --ctx-size N` | `4096` | context size in tokens (`0` = model default) |
| `-b, --batch-size N` | `2048` | logical batch size |
| `-ub, --ubatch-size N` | `512` | physical batch size |
| `-ngl, --n-gpu-layers N` | `-1` | layers to offload to GPU (`-1` = all) |
| `-t, --threads N` | `-1` | generation threads (`-1` = auto) |
| `-tb, --threads-batch N` | — | prompt-processing threads (default: same as `--threads`) |
| `-np, --parallel N` | `1` | accepted for compatibility; requests beyond one queue |
| `-fa, --flash-attn VAL` | `auto` | `on`, `off`, or `auto` |
| `--mlock` | off | lock model memory in RAM |
| `--no-mmap` | off | do not memory-map the model file |
| `-s, --seed N` | random | default RNG seed |
| `--embedding` / `--embeddings` | off | enable `/v1/embeddings` (pooled) |
| `--pooling TYPE` | model default | `none`, `mean`, `cls`, `last`, `rank` |
| `--cache-reuse N` | `1` | min prefix tokens to reuse from KV cache (`0` disables) |
| `-v, --verbose` | off | verbose logging |
| `-h, --help` / `--version` | — | print help / version and exit |

Flags `jx-engine` deliberately does **not** implement or advertise —
`--mmproj`, `--context-shift`, `--reasoning-budget` — are absent from both
the parser and `--help` on purpose (see `src/args.h`'s header comment); this
matters to JX Runtime's adapter, which probes `--help` to decide what to
pass. See [`docs/jx-runtime-integration.md`](docs/jx-runtime-integration.md).

## Roadmap (explicitly not in v1)

- Multimodal input / `--mmproj` (vision projectors)
- True parallel request slots / continuous batching (requests are serialized
  in v1 regardless of `--parallel`)
- Context shift (a context that fills mid-generation stops rather than
  dropping oldest tokens)
- Reasoning-budget control (`--reasoning-budget`)
- Logprobs
- Safetensors models (GGUF only)

## License

MIT. See [`LICENSE`](LICENSE). The vendored llama.cpp source
(`@jxburros/llama-cpp-source`) is its own project, also MIT-licensed,
copyright the ggml/llama.cpp authors — see its `LICENSE` file under
`node_modules/@jxburros/llama-cpp-source/` after installing it.
