[Documentation](../README.md) › [Reference](README.md) › Command line

# Command line

`onyx-engine --help` is the source of truth. This page mirrors `src/args.h` and `src/args.cpp` and adds what the help text has no room for.

**One rule governs this surface:** a flag the engine implements is always listed in `--help`, and a flag that is not in `--help` is never accepted by the parser. JX Runtime's adapter probes `--help` before it passes a flag on a real launch, so a flag that appears in only one of the two would turn a missing feature into a failed load. Any new flag goes into the parser and the help text in the same change. See [the integration notes](../integration/jx-runtime.md#version-and-help-probing).

```text
onyx-engine -m MODEL [options]
```

Only `-m` is required. Anything unrecognized exits with status 1 and names the argument. `--help` and `--version` exit 0. `--version` prints the engine version and the llama.cpp build it was compiled against:

```text
onyx-engine 0.5.2
llama.cpp b11820-d77256999
```

## Model

| Flag | Default | Meaning |
|---|---|---|
| `-m, --model PATH` | required | A GGUF file, or a safetensors model (a Hugging Face directory, or a `.safetensors` file inside one) converted to GGUF on first load. See [safetensors resolution](../architecture/safetensors-conversion.md). |
| `--mmproj PATH` | none | Multimodal projector GGUF. Enables image input, and audio when the projector supports it. Turns `--cache-reuse` and `--context-shift` off. See [multimodal input](../guides/multimodal.md). |
| `--convert-dir DIR` | beside the source model | Where converted safetensors models are cached. |
| `-a, --alias NAME` | file or directory stem | The model id the API reports. Always pass it explicitly when another program will read the id. |
| `--chat-template SOURCE` | the model's own | Override with Jinja source, or the name `chatml`. |
| `--chat-template-file F` | none | Override with a template read from a file. |
| `--jinja` | on | Accepted for compatibility. Jinja is always used. |
| `--no-webui` | off | Accepted for compatibility. There is no web UI to disable. |

When `--alias` is absent the engine strips directories, trailing slashes and a `.gguf` or `.safetensors` suffix from the `-m` value. A decision model, an embedding model and a chat model all take the same flag.

## Network

| Flag | Default | Meaning |
|---|---|---|
| `--host HOST` | `127.0.0.1` | Address to bind. |
| `--port PORT` | `8080` | Port to listen on (1 to 65535). |
| `--api-key KEY` | none | Require `Authorization: Bearer KEY` on every route except `GET /health`. |

The server listens only after the model has loaded. A failure to bind prints `error: failed to bind to HOST:PORT` and exits 1. Read the [security guide](../guides/security.md) before binding anything but loopback.

## Compute

| Flag | Default | Meaning |
|---|---|---|
| `-c, --ctx-size N` | `4096` | Context size in tokens. `0` means the model's own training context. With `-np N` this is **divided** across the slots. |
| `-b, --batch-size N` | `2048` | Logical batch size. |
| `-ub, --ubatch-size N` | `512` | Physical batch size. |
| `-ngl, --n-gpu-layers N`, `--gpu-layers N` | `-1` | Layers to offload to the GPU. `-1` offloads all that fit. Meaningless on a CPU-only build. |
| `-t, --threads N` | `-1` | Generation threads. `-1` is automatic. |
| `-tb, --threads-batch N` | same as `-t` | Prompt-processing threads. |
| `-np, --parallel N` | `1` | Concurrent request slots. Requests beyond `N` queue, first in first out. See [slots and context](../architecture/concurrency.md#slots-and-context). |
| `-fa, --flash-attn VAL` | `auto` | `on`, `off`, `auto`, `1` or `0`. A value is mandatory: a bare `-fa` exits with `-fa requires a value`. |
| `--mlock` | off | Lock the model's memory in RAM. |
| `--no-mmap` | off | Do not memory-map the model file. |
| `-s, --seed N` | random | Default RNG seed. A request's `seed` overrides it. |

## Features

| Flag | Default | Meaning |
|---|---|---|
| `--embedding`, `--embeddings` | off | Serve `/v1/embeddings` and nothing that generates. Forces one slot, disables context shift and sets the physical batch size equal to the logical one. Not allowed with a decision model. |
| `--pooling TYPE` | the model's | `none`, `mean`, `cls`, `last` or `rank`. Requires `--embedding`. |
| `--cache-reuse N` | `1` | Minimum common-prefix length to reuse from a slot's KV cache. `0` turns reuse off. See [KV cache](../architecture/kv-cache.md). |
| `--context-shift`, `--no-context-shift` | off | When a generating slot fills its context, drop the oldest unprotected tokens and keep going instead of finishing with `length`. |
| `--keep N` | `0` | Tokens at the front of the prompt that a shift never drops. `-1` keeps the whole prompt. |
| `--reasoning-budget N` | `-1` | Thinking-token budget: `-1` unrestricted, `0` suppress thinking, `N > 0` force the end of thinking after `N` tokens. A request's `reasoning_budget_tokens` overrides it. See [reasoning budget](../architecture/reasoning-and-logprobs.md). |
| `--reasoning-budget-message MSG` | none | Text injected before the forced closing tag when the budget runs out. |
| `-v, --verbose` | off | Verbose llama.cpp logging. Without it only warnings and errors reach stderr, which keeps a managed process's log readable. |

## Validation at startup

The parser refuses, with a one-line `error:` on stderr and exit status 1:

- a missing `-m`;
- a port outside 1 to 65535, `-np` below 1, `-b` or `-ub` below 1, a negative `--cache-reuse`, or a negative `-c`;
- an unknown `--pooling` value, or `--pooling` without `--embedding`;
- `--embedding` together with a decision model (found at load time);
- a `--chat-template` value that is not Jinja source or `chatml` (use `--chat-template-file` for a path).

## Environment variables

| Variable | Read by | Meaning |
|---|---|---|
| `ONYX_ENGINE_CONVERT_SCRIPT` | the engine | Path to `convert-safetensors.py`, when it is not found beside the binary. |
| `ONYX_BUILD_JOBS` | `npm run build` | Parallel compile jobs. The default is one per CPU. |
| `ONYX_SMOKE_PORT` | `scripts/smoke-test.sh` | Base port for the test instances (default `18190`). |

## Exit status

`0` after `--help` or `--version`, and when the server stops listening normally. `1` for an argument error, a model that will not resolve or load, a bind failure, or a server that terminates unexpectedly.
