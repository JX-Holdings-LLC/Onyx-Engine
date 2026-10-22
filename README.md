<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo/svg/logo-on-dark.svg">
    <img alt="Onyx Engine" src="assets/logo/svg/logo-on-light.svg" width="420">
  </picture>
</p>

# Onyx Engine

One binary. One model. One port. Onyx Engine is a single-model, OpenAI-compatible model server built on [llama.cpp](https://github.com/ggml-org/llama.cpp), and it is built the way a good shop builds an engine: a small number of parts, each doing one job, with nothing bolted on that the machine does not need.

You point it at a model (a GGUF, or a safetensors model it converts once and remembers), it binds `127.0.0.1`, and it serves chat, completions, embeddings and typed decisions over HTTP. When something goes wrong it tells you which part, in plain words, instead of running rough and hoping you do not notice.

![Anatomy of the engine: callers, HTTP layer, request queue, engine loop and slots, llama.cpp and ggml](assets/diagrams/svg/anatomy.svg)

## Why it exists

[JX Runtime](https://github.com/jxburros/JX-Runtime), a Node.js model runtime, runs one engine process per loaded model. Onyx Engine is the process it spawns in place of a third-party `llama-server` binary: JX Runtime spawns it, polls `GET /health` until the model is up, and proxies callers' requests straight through. It also stands on its own for anything that speaks the OpenAI API. The [integration notes](docs/integration/jx-runtime.md) give the exact contract.

## Spec sheet

| | |
|---|---|
| **Serves** | `/v1/chat/completions`, `/v1/completions`, `/v1/embeddings`, `/v1/models`, `/v1/systemone`, plus `/health`, `/props`, `/tokenize`, `/detokenize`, `/apply-template` |
| **Models** | GGUF; safetensors (`LlamaForCausalLM`) converted to GGUF on first load by a numpy-only script, cached and reused; multimodal through `--mmproj`; embedding, reranking and decision models |
| **Concurrency** | `-np N` request slots, one engine thread, one shared batch per tick (continuous batching) |
| **Streaming** | SSE with a closing `usage` and `timings` frame, then `[DONE]` |
| **Constraints** | GBNF grammar, `json_schema`, `response_format`, tool calls with streamed deltas |
| **Extras** | logprobs, reasoning-budget control, context shift, per-slot prefix reuse |
| **Hardware** | CPU by default; CUDA, Vulkan, Metal, ROCm/HIP or BLAS chosen at build time |
| **Platforms** | `linux-x64`, `darwin-arm64`, `darwin-x64`, `win32-x64` |
| **Built on** | llama.cpp b11820 (`d77256999`), fetched by exact commit and verified with Git |
| **Auth** | optional `--api-key` bearer token, on every route but `/health` |
| **Licence** | MIT |

## Start her up

**Prebuilt.** Each [release](https://github.com/JX-Holdings-LLC/Onyx-Engine/releases) carries an archive per platform and a `checksums.txt`. The prebuilt binaries are CPU-only, and the x64 ones need AVX2.

**From source.** CMake 3.16+, a C++17 compiler, Git, and Node.js (npm is only the installer; the package has no dependencies).

```bash
git clone https://github.com/JX-Holdings-LLC/Onyx-Engine.git
cd Onyx-Engine
npm ci            # fetches the pinned llama.cpp commit with Git
npm run build     # CMake configure and build, one job per CPU

./build/onyx-engine -m model.gguf --port 8080
```

The port stays shut until the model has loaded, so the first answer from `/health` is the sound of an engine that has actually caught. Then:

```bash
curl http://127.0.0.1:8080/health

curl http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"Hello!"}]}'
```

Build with a GPU backend, size the context, run a safetensors model, or put a key on it: [getting started](docs/getting-started.md) and [serving models](docs/guides/serving-models.md) go through each.

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
| `GET` | `/health` | Liveness; exempt from `--api-key` |
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
| `--host`, `--port` | `127.0.0.1`, `8080` | Where to listen |
| `--api-key KEY` | none | Require a bearer token (except `/health`) |
| `-c, --ctx-size N` | `4096` | Context in tokens; `0` is the model's own. Divided across slots |
| `-np, --parallel N` | `1` | Concurrent request slots |
| `-ngl, --n-gpu-layers N` | `-1` | Layers on the GPU (`-1` is all that fit) |
| `--mmproj PATH` | none | Multimodal projector: images, and audio if it can |
| `--embedding`, `--pooling TYPE` | off | Serve embeddings (or rerank, with `rank`) |
| `--context-shift`, `--keep N` | off, `0` | Keep generating when a slot fills, protecting the first `N` tokens |
| `--cache-reuse N` | `1` | Smallest prefix worth reusing; `0` turns it off |
| `--reasoning-budget N` | `-1` | Bound a thinking model's `<think>` block |

One rule governs the whole panel: a flag the engine implements is always in `--help`, and a flag that is not in `--help` is never accepted. A caller that probes `--help` before passing a flag, as JX Runtime does, can trust what it finds.

## Tolerances

What it will not do, stated up front rather than found out on a Friday afternoon:

- **One model per process.** A second model is a second process on its own port.
- **No speculative decoding yet.** Not planned at all: multi-model routing, built-in tools or MCP, a web UI, TTS or realtime.
- **A narrow converter.** Safetensors means Hugging Face `LlamaForCausalLM` with a byte-level BPE `tokenizer.json`. Anything else is refused with a reason.
- **Strict about fields.** `logit_bias`, `mirostat`, `dry_*`, `xtc_*`, `typical_p`, `n` above 1 and the like answer `400`; they are never quietly ignored.
- **Media only inline,** as `data:` URIs or base64. No video.
- **Plain HTTP.** Terminate TLS in front of it, and read the [security guide](docs/guides/security.md) before binding anything but loopback.

The full list, with the exact error for each, is [compatibility and limits](docs/reference/compatibility.md). Where the design is headed is in the [roadmap](ROADMAP.md).

## The shop manual

| Start with | For |
|---|---|
| [Getting started](docs/getting-started.md) | Binary or source, first request, first stream |
| [Guides](docs/guides/README.md) | Serving, structured output and tools, multimodal, embeddings, decision models, security, troubleshooting |
| [Reference](docs/reference/README.md) | Command line, every endpoint, compatibility and limits |
| [Architecture](docs/architecture/README.md) | Concurrency, KV cache, multimodal, decision models, conversion, build layering |
| [Integration](docs/integration/jx-runtime.md) | The JX Runtime contract |
| [Development](docs/development/README.md) | Building, testing, vendoring, releasing, maintaining these docs |
| [Roadmap](ROADMAP.md), [changelog](CHANGELOG.md) | Where it is going, where it has been |

The [documentation home](docs/README.md) has the full index.

## Maintenance schedule

```bash
npm test                  # offline smoke test on tiny generated models: every endpoint, slots,
                          # context shift, multimodal, logprobs, reasoning budget, decision models
bash scripts/safetensors-test.sh    # the converter and its cache, end to end
```

CI builds on Linux, macOS and Windows for every change and runs both tests where it can. Releases are cut by hand from `main` with a dry run first; a failed platform blocks the whole release, so a partial set never ships. See [testing and CI](docs/development/testing-and-ci.md) and [releasing](docs/development/releasing.md).

## Brand

The mark is a hex nut seen from above with a cyan-lit bore: a small, solid part that the rest of the machine is bolted to. Logos, the app icon and the colours are in [`assets/logo/`](assets/logo/) and [`docs/brand.md`](docs/brand.md). The documentation diagrams are drawn in the same palette with [Vixl](https://github.com/jxburros/Vixl); [`assets/diagrams/`](assets/diagrams/) has the script.

## Licence

MIT. See [`LICENSE`](LICENSE). The vendored llama.cpp source (`@jxburros/llama-cpp-source`) is its own project, also MIT-licensed, copyright the ggml/llama.cpp authors; its `LICENSE` file sits under `node_modules/@jxburros/llama-cpp-source/` after `npm ci`. [`third_party/cpp-httplib`](third_party/cpp-httplib) is a verbatim copy of [cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.54.1 (MIT, copyright Yuji Hirose); its licence and provenance notes are in that directory.
