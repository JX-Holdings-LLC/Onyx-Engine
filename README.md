<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo/svg/logo-on-dark.svg">
    <img alt="Onyx Engine" src="assets/logo/svg/logo-on-light.svg" width="420">
  </picture>
</p>

# Onyx Engine

One binary. One model. One port. Onyx Engine is a small, strict, supervisable OpenAI-compatible model server built on [llama.cpp](https://github.com/ggml-org/llama.cpp), for people who put a local model inside their own app or service. It is built the way a good shop builds an engine: a small number of parts, each doing one job, with nothing bolted on that the machine does not need.

You point it at a model (a GGUF, or a safetensors model it converts once and remembers), it binds `127.0.0.1`, and it serves chat, completions, embeddings and **typed decisions** over HTTP. Decision models are the part you will not find elsewhere: ask a `choice`, `score` or yes/no question about a piece of text and get calibrated probabilities from one prefill, on `POST /v1/systemone` ([decision models](docs/guides/decision-models.md)). When something goes wrong it tells you which part, in plain words, instead of running rough and hoping you do not notice.

![Anatomy of the engine: callers, HTTP layer, request queue, engine loop and slots, llama.cpp and ggml](assets/diagrams/svg/anatomy.svg)

## Why you might want it

- **Supervisable.** It answers `503 Loading model` from the moment it starts and `200` once the model is ready, so your program can tell loading from crashed. `--port 0` picks a free port and reports it. The `--help` text lists exactly the flags the build accepts, so a supervisor can probe before it passes one. [Supervising Onyx](docs/guides/supervising.md) has worked examples in Node, Python and shell.
- **Strict.** Fields it does not implement get a `400`, not silent indifference. Media arrives inline; it never fetches a URL or opens a file on a request's behalf. It refuses browsers, and a loopback server refuses foreign `Host` names. It will not bind beyond loopback without a key.
- **Predictable.** One model per process, one engine loop, `-np N` slots with continuous batching, an optional queue limit that answers `503` with `Retry-After`, and Prometheus metrics under llama.cpp's own names. What stays stable between versions is written down on the [stability page](docs/reference/stability.md).

## Spec sheet

| | |
|---|---|
| **Serves** | `/v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, `/v1/models`, `/v1/systemone`, plus `/health`, `/props`, `/metrics`, `/tokenize`, `/detokenize`, `/apply-template` |
| **Models** | GGUF; safetensors (`LlamaForCausalLM`) converted to GGUF on first load by a numpy-only script, cached and reused; multimodal through `--mmproj`; embedding, reranking and decision models |
| **Concurrency** | `-np N` request slots, one engine thread, one shared batch per tick (continuous batching, decode rows first); `--max-queue`, `--queue-timeout` and `--tick-tokens` to bound waiting and latency |
| **Streaming** | SSE with a closing `usage` and `timings` frame, then `[DONE]` |
| **Constraints** | GBNF grammar, `json_schema`, `response_format` (a length cut is a `400`, never half an object), tool calls with streamed deltas |
| **Memory** | KV cache types (`-ctk`/`-ctv`, e.g. `q8_0`), MoE expert offload (`--cpu-moe`, `--n-cpu-moe`) and tensor placement (`-ot`) |
| **Extras** | logprobs, reasoning-budget control, context shift, per-slot prefix reuse (sliding-window and recurrent models included) |
| **Operations** | `503` while loading, `--port 0`, `X-Request-Id` echo, `/metrics` (`llamacpp:*`), `--max-queue` and `--queue-timeout` with `Retry-After` |
| **Hardware** | CPU by default; CUDA, Vulkan, Metal, ROCm/HIP or BLAS chosen at build time; prebuilt Vulkan archives for Linux and Windows |
| **Platforms** | `linux-x64`, `linux-arm64`, `darwin-arm64`, `darwin-x64`, `win32-x64`, plus `-baseline` (no AVX2) and `-vulkan` variants for Linux and Windows x64; a Docker image |
| **Built on** | llama.cpp b11820 (`d77256999`), fetched by exact commit and verified with Git |
| **Auth** | optional bearer token (`--api-key`, `--api-key-file` or `ONYX_ENGINE_API_KEY`; constant-time compare), on every route but `/health`; required for a non-loopback `--host` |
| **Licence** | MIT |

## Start her up

**Docker.** Mount your models and go:

```bash
docker run --rm -p 127.0.0.1:8080:8080 -v "$HOME/models:/models:ro" \
    ghcr.io/jx-holdings-llc/onyx-engine:latest -m /models/model.gguf --allow-unauthenticated
```

`--allow-unauthenticated` is there because the container listens on all interfaces; the port is published on loopback only. Use `--api-key` instead when anything else can reach it. See [Docker](docs/guides/docker.md).

**Prebuilt.** Each [release](https://github.com/JX-Holdings-LLC/Onyx-Engine/releases) carries an archive per platform and a `checksums.txt`. Plain x64 archives need AVX2 (take `-baseline` otherwise); `-vulkan` archives use any GPU with a Vulkan driver.

**From source.** CMake 3.16+, a C++17 compiler and Git. No Node.

```bash
git clone https://github.com/JX-Holdings-LLC/Onyx-Engine.git
cd Onyx-Engine
cmake -B build -DCMAKE_BUILD_TYPE=Release     # fetches the pinned llama.cpp commit with Git
cmake --build build --target onyx-engine -j 8

./build/onyx-engine -m model.gguf --port 8080
```

`cmake --preset vulkan` (or `cuda`, `metal`, `hip`) builds with a GPU backend; `npm ci && npm run build` still works if you prefer it. The port opens at once: `/health` answers `503` while the model loads and `200` the moment the engine has actually caught. Then:

```bash
curl http://127.0.0.1:8080/health

curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello!"}]}'
```

Build with a GPU backend, size the context, run a safetensors model, or put a key on it: [getting started](docs/getting-started.md) and [serving models](docs/guides/serving-models.md) go through each.

## Using it with other clients

Anything that speaks the OpenAI API works with the base URL `http://127.0.0.1:8080/v1` and the model name you gave `--alias`. Pass any non-empty API key if you did not set one.

| Client | Notes |
|---|---|
| `openai` for Python | Chat, streaming, completions, logprobs, `json_schema` and embeddings checked against `openai` 3.36. Embeddings come back base64 by default, which Onyx serves. |
| `openai` for JavaScript | Chat, streaming and embeddings checked. |
| `curl` | Add `"stream": true` and `curl -N` to stream; the last frame carries `usage` and `timings`. |
| Open WebUI, Continue | Configure an OpenAI-compatible connection. Not tested by this project; Open WebUI in Docker needs the engine bound to an address it can reach, with a key. |

Strict field handling means a client that sends `logit_bias`, `mirostat`, `dry_*`, `xtc_*`, or `n` above 1 gets a `400` naming the field; turn that setting off in the client. [Using it with other clients](docs/guides/clients.md) has the code.

## What goes on under the hood

A request comes in the intake (the HTTP layer), gets authenticated, validated and run through the chat template, queues up, and is admitted to a free slot. Every slot with work contributes rows to one shared batch; one decode serves them all; each slot is sampled on its own and streamed out the exhaust.

![One request, start to finish: authenticate, validate, render, tokenize, queue, decode, stream](assets/diagrams/svg/request-lifecycle.svg)

A few things worth knowing before you open the bonnet any further:

- **Slots share the context.** `-c 16384 -np 4` gives each request about 4096 tokens, not 16384. `/props` reports the per-slot figure as `n_ctx`, because that is the number a prompt has to fit. See [slots and context](docs/architecture/concurrency.md#slots-and-context).
- **Repeated prefixes are cheap.** Each slot keeps the tokens it last decoded and reuses the common prefix of the next prompt. `timings.cache_n` tells you how much it saved.
- **Images stall the loop, briefly.** Encoding media happens inline at admission and holds the engine for that one prefill. [Why](docs/architecture/multimodal.md).
- **It does no I/O on your behalf.** No URL or file named in a request is ever opened; media arrives as base64. Browsers are refused outright.

The whole machine, part by part, is in the [architecture notes](docs/architecture/README.md).

## Routes

| Method | Path | What it does |
|---|---|---|
| `GET` | `/health` | `503` while loading, `200` when ready; exempt from `--api-key` |
| `GET` | `/metrics` | Prometheus metrics (`llamacpp:*`); only with `--metrics` |
| `GET` | `/props` | Model and build metadata, per-slot `n_ctx`, modalities |
| `GET` | `/v1/models` | The one model, under its alias |
| `POST` | `/tokenize`, `/detokenize` | Text to token ids and back |
| `POST` | `/apply-template` | Render a chat prompt without generating |
| `POST` | `/v1/chat/completions` | Chat, streaming or not, with tools, structured output, logprobs, images |
| `POST` | `/v1/completions` | Raw text completion |
| `POST` | `/v1/embeddings` | Pooled embeddings; only with `--embedding` |
| `POST` | `/v1/systemone` | Typed decisions; only for a decision model |

A process runs in one mode, set at launch, and answers `501` for routes outside it:

![A matrix of the four route groups against text, embedding and decision-model processes](assets/diagrams/svg/route-modes.svg)

Every field, shape and error is in the [API reference](docs/reference/api/README.md).

## The controls

The flags you will touch most. `onyx-engine --help` is the source of truth, and the [command-line reference](docs/reference/cli.md) has the rest.

| Flag | Default | Does |
|---|---|---|
| `-m, --model PATH` | required | GGUF, or a safetensors model converted on first load |
| `-a, --alias NAME` | file stem | The model id the API reports |
| `--host`, `--port` | `127.0.0.1`, `8080` | Where to listen; `--port 0` picks a free port. A non-loopback host needs a key |
| `--api-key KEY`, `--api-key-file PATH` | none | Require a bearer token (except `/health`); the file (or `ONYX_ENGINE_API_KEY`) keeps it out of the process list |
| `--timeout N`, `--max-request-bytes N` | `600`, 100 MiB | Write timeout (the read timeout stays 30 s or less); request body cap, `413` beyond |
| `--max-queue N` | `0` (no limit) | Requests allowed to wait for a slot; beyond it, `503` with `Retry-After` |
| `--queue-timeout N` | `0` (no limit) | Seconds a request may wait for a slot; beyond it, `503` with `Retry-After` |
| `--metrics` | off | Serve `/metrics` |
| `-c, --ctx-size N` | `4096` | Context in tokens; `0` is the model's own. Divided across slots |
| `-np, --parallel N` | `1` | Concurrent request slots |
| `-ngl, --n-gpu-layers N` | `-1` | Layers on the GPU (`-1` is all that fit) |
| `-ctk`, `-ctv TYPE` | `f16` | KV cache types; `q8_0` roughly halves KV memory |
| `--cpu-moe`, `--n-cpu-moe N`, `-ot P=B` | off | Keep MoE experts (all, or the first `N` layers) in RAM; place any tensor by regex |
| `--mmproj PATH` | none | Multimodal projector: images, and audio if it can |
| `--embedding`, `--pooling TYPE` | off | Serve embeddings (or rerank, with `rank`) |
| `--context-shift`, `--keep N` | off, `0` | Keep generating when a slot fills, protecting the first `N` tokens |
| `--cache-reuse N` | `1` | Smallest prefix worth reusing; `0` turns it off |
| `--reasoning-budget N` | `-1` | Bound a thinking model's `<think>` block |

One rule governs the whole panel: a flag the engine implements is always in `--help`, and a flag that is not in `--help` is never accepted. A supervisor that probes `--help` before passing a flag can trust what it finds.

## Tolerances

What it will not do, stated up front rather than found out on a Friday afternoon:

- **One model per process.** A second model is a second process on its own port.
- **No speculative decoding yet.** Not planned at all: multi-model routing, built-in tools or MCP, a web UI, TTS or realtime.
- **A narrow converter.** Safetensors means Hugging Face `LlamaForCausalLM` with a byte-level BPE `tokenizer.json`. Anything else is refused with a reason.
- **Strict about fields.** `logit_bias`, `mirostat`, `dry_*`, `xtc_*`, `typical_p`, `n` above 1 and the like answer `400`; they are never quietly ignored.
- **Media only inline,** as `data:` URIs or base64. No video.
- **Plain HTTP.** Terminate TLS in front of it, and read the [security guide](docs/guides/security.md) before binding anything but loopback.
- **No model downloads, no UI, no automatic GPU setup.** It serves what you point it at.

The full list, with the exact error for each, is [compatibility and limits](docs/reference/compatibility.md). Where the design is headed is in the [roadmap](ROADMAP.md).

## Works best with JX Runtime

[JX Runtime](https://github.com/jxburros/JX-Runtime), a Node.js model runtime, is the first-class integration: it spawns one `onyx-engine` per loaded model, waits on `/health`, probes `--help` before passing flags, installs the release archives for you (`jx-runtime backend install --engine onyx`), and proxies callers' requests straight through. It gets no private contract: everything it relies on is public behaviour on the [stability page](docs/reference/stability.md). The [integration notes](docs/integration/jx-runtime.md) give the exact contract.

## The shop manual

| Start with | For |
|---|---|
| [Getting started](docs/getting-started.md) | Binary, Docker or source, first request, first stream |
| [Guides](docs/guides/README.md) | Serving, clients, Docker, supervising, structured output and tools, multimodal, embeddings, decision models, security, troubleshooting |
| [Reference](docs/reference/README.md) | Command line, every endpoint, compatibility and limits, stability |
| [Architecture](docs/architecture/README.md) | Concurrency, KV cache, multimodal, decision models, conversion, build layering |
| [Integration](docs/integration/jx-runtime.md) | The JX Runtime contract |
| [Development](docs/development/README.md), [contributing](CONTRIBUTING.md) | Code map, building, testing, vendoring, releasing, maintaining these docs |
| [Roadmap](ROADMAP.md), [changelog](CHANGELOG.md) | Where it is going, where it has been |

The [documentation home](docs/README.md) has the full index.

## Maintenance schedule

```bash
ctest --test-dir build              # C++ unit tests: argument parsing, request checks, model resolution
bash scripts/smoke-test.sh          # offline end-to-end test on tiny generated models: every endpoint,
                                    # slots, context shift, multimodal, logprobs, reasoning budget,
                                    # decision models, readiness, queue limits, metrics, auth
bash scripts/safetensors-test.sh    # the converter and its cache, end to end
```

CI builds on Linux, macOS and Windows for every change and runs the unit tests and the smoke test on all three, plus a Node-free build from a bare clone. [`CONTRIBUTING.md`](CONTRIBUTING.md) gets you from a clone to a pull request. Releases are cut by hand from `main` with a dry run first; a failed platform blocks the whole release, so a partial set never ships. See [testing and CI](docs/development/testing-and-ci.md) and [releasing](docs/development/releasing.md).

## Brand

The mark is a hex nut seen from above with a cyan-lit bore: a small, solid part that the rest of the machine is bolted to. Logos, the app icon and the colours are in [`assets/logo/`](assets/logo/) and [`docs/brand.md`](docs/brand.md). The documentation diagrams are drawn in the same palette with [Vixl](https://github.com/jxburros/Vixl); [`assets/diagrams/`](assets/diagrams/) has the script.

## Licence

MIT. See [`LICENSE`](LICENSE). The vendored llama.cpp source (`@jxburros/llama-cpp-source`) is its own project, also MIT-licensed, copyright the ggml/llama.cpp authors; its `LICENSE` file sits in `.deps/llama.cpp/` after a CMake fetch, or under `node_modules/@jxburros/llama-cpp-source/` after `npm ci`, and ships in every archive as `LICENSE-llama.cpp`. [`third_party/cpp-httplib`](third_party/cpp-httplib) is a verbatim copy of [cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.62.0 (MIT, copyright Yuji Hirose); its licence and provenance notes are in that directory. [`third_party/nlohmann`](third_party/nlohmann) is [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 (MIT, copyright Niels Lohmann), the copy the pinned llama.cpp vendors; its licence and notes are in that directory.
