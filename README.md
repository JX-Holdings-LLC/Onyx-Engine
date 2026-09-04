# Onyx Engine

Onyx Engine is a single-model, OpenAI-compatible model-serving binary built on
[llama.cpp](https://github.com/ggml-org/llama.cpp) (vendored as a pinned
source tree, currently `v0.3.0-90-g9723942ad`, distributed as the npm package
`@jxburros/llama-cpp-source`). One `onyx-engine` process loads one model
(GGUF, or a safetensors model converted to GGUF on first load) and serves
it over HTTP on `127.0.0.1:<port>`.

## Relationship to JX Runtime

Onyx Engine exists so [JX Runtime](https://github.com/jxburros/JX-Runtime) — a
Node.js model runtime — can spawn `onyx-engine` in place of a third-party
`llama-server` binary: one `onyx-engine` process per loaded model, polled at
`GET /health`, with its OpenAI-compatible endpoints proxied through to
callers. See [`docs/jx-runtime-integration.md`](docs/jx-runtime-integration.md)
for exactly how the two projects are meant to connect; that mapping is a
description of the intended contract, not a claim that JX Runtime has an
adapter for Onyx Engine today (it does not — see that document's last section).

## Features (v2)

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
- **Parallel request slots + continuous batching** (`--parallel`/`-np`): a
  single engine thread packs one shared `llama_batch` per tick across all
  slots, so up to `-np N` requests generate concurrently instead of queuing
  one at a time
- **Context shift** (`--context-shift`, `--keep`): a generating slot that
  fills its context drops the oldest non-preserved tokens and keeps going
  instead of stopping, mirroring `llama-server`
- **Logprobs** on both `/v1/chat/completions` (`logprobs`/`top_logprobs`,
  nested `logprobs.content[]` shape) and `/v1/completions` (legacy
  `logprobs: N`, flat parallel-array shape) — raw model probabilities from
  the full-vocab softmax, unaffected by grammar/sampler constraints
- **Reasoning-budget control** (`--reasoning-budget`,
  `--reasoning-budget-message`, or the per-request `reasoning_budget_tokens`):
  cap or suppress a thinking model's `<think>...</think>` block
- **Multimodal input** via `--mmproj` (image, and audio when the projector
  supports it): `image_url`/`input_audio` content parts as `data:` URIs or
  raw base64 only — onyx-engine never fetches a remote or local resource on a
  request's behalf
- **Safetensors models**: a `-m` pointing at a Hugging Face directory (or a
  `.safetensors` file inside one) is converted to GGUF on first load by
  onyx-engine's own numpy-only converter (`scripts/convert-safetensors.py`,
  no torch/transformers), cached under `onyx-cache/` next to the model (or
  `--convert-dir`) and reused on later launches
- KV-cache common-prefix reuse (`--cache-reuse`), per slot, to avoid
  re-processing an unchanged prompt prefix
- CPU by default; optional CUDA, Vulkan, Metal, ROCm/HIP, or BLAS
  acceleration selected at build time via CMake options
- Optional bearer-token auth (`--api-key`) on every endpoint except `/health`

**v2 scope and limits**, stated up front rather than discovered later:

- One model per process, still. Running a second model means starting a
  second `onyx-engine` process on a different port.
- `-np N` divides the context N ways (`kv_unified` stays at llama.cpp's
  default `false`), so more slots means less context per request; an
  overflowing slot without `--context-shift` simply stops at `n_predict`
  as before.
- A multimodal request's media is prefilled through mtmd at slot admission,
  which runs the vision/audio encoder and its own decode calls inline —
  this momentarily serializes the batching loop for the duration of that
  one prefill (see [`docs/architecture.md`](docs/architecture.md)), but
  other slots' KV and queued requests are otherwise unaffected.
- `--context-shift` and `--cache-reuse` are force-disabled (with a startup
  warning) whenever `--mmproj` is loaded — a media chunk's KV positions
  cannot be partially discarded or prefix-matched token-by-token.
- Remote/local media URLs (`http://`, `https://`, `file://`) are rejected
  with `400` by design; only `data:<mime>;base64,...` URIs or bare base64
  strings are accepted, since onyx-engine performs no I/O on a request's
  behalf.
- The safetensors converter only handles the standard Hugging Face
  `LlamaForCausalLM` layout (optionally sharded via
  `model.safetensors.index.json`) with a byte-level BPE `tokenizer.json`.
  Anything else — another architecture, a SentencePiece
  `tokenizer.model`, exotic dtypes — is refused with a clear error rather
  than silently mishandled.

See [Roadmap](#roadmap) for what remains genuinely out of scope.

## Quick Start

```bash
git clone <this-repo>
cd Onyx-Engine
npm ci   # or: npm install

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target onyx-engine -j

./build/onyx-engine -m model.gguf --port 8080
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

Onyx Engine's only external build input is llama.cpp's C/C++ source tree,
distributed as the npm package `@jxburros/llama-cpp-source` — a pinned,
pruned copy of llama.cpp (not a Node.js library). Using the npm registry as
that channel, rather than a git submodule, means the build has exactly one
trusted external source to fetch from and verify, instead of two (git +
npm). `npm ci` installs it into `node_modules`, and `CMakeLists.txt` picks it
up from there automatically. See
["What llama.cpp's own build is told to skip"](docs/building.md#what-llamacpps-own-build-is-told-to-skip)
and the [architecture doc](docs/architecture.md#build-layering) for the full
source-resolution order (npm package → `-DONYX_ENGINE_LLAMA_DIR` override →
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

`onyx-engine --help` is the source of truth; the flags below mirror
`src/args.h`/`src/args.cpp`.

| Flag | Default | Meaning |
|---|---|---|
| `-m, --model PATH` | (required) | GGUF file, or a safetensors model (HF directory or `.safetensors` file) converted to GGUF on first load |
| `--mmproj PATH` | — | multimodal projector GGUF (enables image, and audio if the projector supports it) |
| `--convert-dir DIR` | alongside the source model | cache directory for converted safetensors models |
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
| `-np, --parallel N` | `1` | concurrent request slots; splits the context N ways |
| `-fa, --flash-attn VAL` | `auto` | `on`, `off`, or `auto` |
| `--mlock` | off | lock model memory in RAM |
| `--no-mmap` | off | do not memory-map the model file |
| `-s, --seed N` | random | default RNG seed |
| `--embedding` / `--embeddings` | off | enable `/v1/embeddings` (pooled) |
| `--pooling TYPE` | model default | `none`, `mean`, `cls`, `last`, `rank` |
| `--cache-reuse N` | `1` | min prefix tokens to reuse from KV cache (`0` disables) |
| `--context-shift` / `--no-context-shift` | off | drop oldest tokens instead of stopping when a slot's context fills |
| `--keep N` | `0` | tokens at the front preserved by a context shift (`-1` = whole prompt) |
| `--reasoning-budget N` | `-1` | thinking-token budget: `-1` unrestricted, `0` suppress thinking, `N > 0` force the end of thinking after `N` tokens |
| `--reasoning-budget-message MSG` | — | text injected before the forced end-of-thinking tag when the budget runs out |
| `-v, --verbose` | off | verbose logging |
| `-h, --help` / `--version` | — | print help / version and exit |

`onyx-engine` follows one rule for its CLI surface: a flag it implements is
always advertised in `--help`, and a flag not in `--help` is never accepted
by the parser either (see `src/args.h`'s header comment). This matters to
JX Runtime's adapter, which probes `--help` before ever passing a flag on a
real launch (`supportsFlag`) rather than assuming a capability exists. As of
v2 that probe for `--mmproj`, `--context-shift`, `--reasoning-budget`, and
`--parallel` comes back `true` — all four are real, not placeholders. See
[`docs/jx-runtime-integration.md`](docs/jx-runtime-integration.md).

## Roadmap (still out of scope)

- Multi-model per process (run a second `onyx-engine` process instead)
- Speculative decoding
- Audio/image input beyond what the loaded `--mmproj` projector supports;
  video is off by design (`MTMD_VIDEO=OFF` — it would shell out to `ffmpeg`
  at runtime, a dependency onyx-engine does not take)
- Fetching remote or local media by URL — only inline `data:`/base64 payloads
  are accepted, since onyx-engine performs no I/O on a request's behalf
- Safetensors architectures other than HF `LlamaForCausalLM`, and tokenizers
  other than a byte-level BPE `tokenizer.json` (e.g. SentencePiece)

## License

MIT. See [`LICENSE`](LICENSE). The vendored llama.cpp source
(`@jxburros/llama-cpp-source`) is its own project, also MIT-licensed,
copyright the ggml/llama.cpp authors — see its `LICENSE` file under
`node_modules/@jxburros/llama-cpp-source/` after installing it.

[`third_party/cpp-httplib`](third_party/cpp-httplib) is a verbatim copy of
[cpp-httplib](https://github.com/yhirose/cpp-httplib) v0.54.1 (MIT, copyright
Yuji Hirose); its `LICENSE` and provenance notes are in that directory.
