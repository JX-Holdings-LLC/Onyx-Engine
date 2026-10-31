[Documentation](../README.md) › [Reference](README.md) › Command line

# Command line

`onyx-engine --help` is the source of truth. This page mirrors `src/args.h` and `src/args.cpp` and adds what the help text has no room for.

**One rule governs this surface:** a flag the engine implements is always listed in `--help`, and a flag that is not in `--help` is never accepted by the parser. A supervisor can probe `--help` before it passes a flag on a real launch (JX Runtime's adapter does), so a flag that appears in only one of the two would turn a missing feature into a failed load. Any new flag goes into the parser and the help text in the same change. The [stability page](stability.md) says which parts of this surface are stable.

```text
onyx-engine -m MODEL [options]
```

Only `-m` is required. Anything unrecognized exits with status 1 and names the argument. `--help` and `--version` exit 0. `--version` prints the engine version and the llama.cpp build it was compiled against:

```text
onyx-engine 0.7.0
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
| `--host HOST` | `127.0.0.1` | Address to bind. Anything but a loopback address (`127.x.x.x`, `localhost`, `::1`) requires `--api-key` or `--allow-unauthenticated`. |
| `--port PORT` | `8080` | Port to listen on (0 to 65535). `0` lets the operating system pick a free port; the bound port is printed on stderr and reported by `/props` as `port`. |
| `--api-key KEY` | none | Require `Authorization: Bearer KEY` on every route except `GET /health`. The comparison takes the same time however much of the key matches. The key shows in the process list; prefer `--api-key-file` or `ONYX_ENGINE_API_KEY`. |
| `--api-key-file PATH` | none | Read the key from a file instead: its contents with one trailing newline (LF or CRLF) dropped. A file that cannot be read or holds no key exits 1, and so does passing it together with `--api-key`. |
| `--timeout N` | `600` | Seconds a response may wait to be written (the write timeout; a stream can go quiet while a long prompt is processed), and the ceiling of the read timeout, which is 30 seconds or `N`, whichever is less. At least `1`. llama-server's flag of the same name sets both to `N`; Onyx keeps its short read timeout so slow or idle clients cannot hold the worker threads. |
| `--max-request-bytes N` | `104857600` (100 MiB) | Largest request body accepted; a larger one gets `413` before it is read. At least `1`. Inline base64 media are the only reason to keep it high; a text-only deployment can lower it. |
| `--allow-unauthenticated` | off | Allow a non-loopback `--host` without `--api-key`. Without it, that combination exits 1 before anything is bound. |
| `--metrics` | off | Serve Prometheus metrics on `GET /metrics` (behind `--api-key` like any other route). See [service endpoints](api/service-endpoints.md#get-metrics). |

The server binds and starts answering before the model loads. Until loading finishes, `/health` and every other route answer `503` with `{"error":{"code":503,"message":"Loading model","type":"unavailable_error"}}`. Two stderr lines mark the transitions, and both carry the bound port:

```text
onyx-engine listening on http://127.0.0.1:8080 (loading)
onyx-engine ready on http://127.0.0.1:8080 (model: my-model, ctx: 4096)
```

A failure to bind prints `error: failed to bind to HOST:PORT` and exits 1. A failed load prints its `error:` line, closes the port and exits 1. Read the [security guide](../guides/security.md) before binding anything but loopback.

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
| `--max-queue N` | `0` | Requests allowed to wait for a slot. When `N` are already waiting, a new generation request gets `503` (`unavailable_error`) with `Retry-After: 1`. `0` means no limit. Applies to chat and text completions. |
| `--queue-timeout N` | `0` | Seconds a generation request may wait for a slot. A request still queued after `N` seconds gets `503` (`unavailable_error`) with `Retry-After: 1`, the same answer as `--max-queue`; a streaming request that already opened its stream gets that error as its last `data:` frame instead. `0` means wait as long as it takes. Counted by `onyx:requests_timed_out_total` in `/metrics`. |
| `--tick-tokens N` | `0` | Tokens per batching tick, decode rows included. Generating slots always get their next token first; prompt chunks share what is left, at least one token per tick. `0` uses `--batch-size`; a larger value is capped at it. See [the tick budget](../architecture/concurrency.md#the-tick-budget). |
| `-fa, --flash-attn VAL` | `auto` | `on`, `off`, `auto`, `1` or `0`. A value is mandatory: a bare `-fa` exits with `-fa requires a value`. |
| `--mlock` | off | Lock the model's memory in RAM. |
| `--no-mmap` | off | Do not memory-map the model file. |
| `-s, --seed N` | random | Default RNG seed. A request's `seed` overrides it. |

## Memory

| Flag | Default | Meaning |
|---|---|---|
| `-ctk, --cache-type-k TYPE` | `f16` | Element type of the K cache: `f32`, `f16`, `bf16`, `q8_0`, `q4_0`, `q4_1`, `iq4_nl`, `q5_0` or `q5_1` (llama-server's names, lower case). |
| `-ctv, --cache-type-v TYPE` | `f16` | Element type of the V cache, same values. A quantized V cache needs flash attention: under `-fa auto` llama.cpp turns it on, and `-fa off` with a quantized V type exits 1. |
| `-cmoe, --cpu-moe` | off | Keep every Mixture-of-Experts expert tensor in host memory, so only the dense weights and the KV cache take GPU memory. |
| `-ncmoe, --n-cpu-moe N` | `0` | Keep the expert tensors of the first `N` layers in host memory. `--cpu-moe` covers all of them. |
| `-ot, --override-tensor P=B` | none | Place every tensor whose name matches the regular expression `P` in the buffer type `B` (`CPU`, or a device's own type such as `CUDA0` or `Vulkan0`). Comma-separate several pairs, or repeat the flag; the first matching pattern wins, and explicit `-ot` pairs come before the ones `--cpu-moe` and `--n-cpu-moe` add. An unknown buffer type fails the load and lists the known ones. |

`q8_0` for both K and V roughly halves KV memory against `f16` with very little quality loss on most models; it is the type to reach for first. `q4_0` and the other 4-bit types save more but hurt some models badly, sliding-window models (Gemma) in particular. A quantized type also needs the model's head size to be a multiple of its block size (32 for `q8_0`); when it is not, the load fails with `failed to create inference context (... cache types K/V)`.

The MoE flags match tensors named `blk.N.ffn_{up,down,gate,gate_up}_{ch,}exps`, llama-server's pattern, so they do nothing on a dense model. Expert tensors kept on the CPU are read through the memory map unless `--no-mmap` is set; llama.cpp prints a warning suggesting it.

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
- a port outside 0 to 65535, `-np` below 1, `-b` or `-ub` below 1, a negative `--cache-reuse`, `--max-queue`, `--queue-timeout`, `--tick-tokens`, `--n-cpu-moe` or `-c`, a `--timeout` or `--max-request-bytes` below 1;
- an `--api-key-file` that cannot be read or holds no key, or one passed together with `--api-key`;
- a `--cache-type-k` or `--cache-type-v` outside the list above, a quantized `--cache-type-v` with `-fa off`, or an `--override-tensor` item that is not `PATTERN=BUFFER_TYPE`;
- a non-loopback `--host` without `--api-key` or `--allow-unauthenticated`;
- an unknown `--pooling` value, or `--pooling` without `--embedding`;
- `--embedding` together with a decision model, or an `--override-tensor` buffer type no device provides (both found at load time);
- a `--chat-template` value that is not Jinja source or `chatml` (use `--chat-template-file` for a path).

## Environment variables

| Variable | Read by | Meaning |
|---|---|---|
| `ONYX_ENGINE_API_KEY` | the engine | The API key, when neither `--api-key` nor `--api-key-file` is given. An empty value means no key. It counts for the non-loopback `--host` rule like the flag. |
| `ONYX_ENGINE_CONVERT_SCRIPT` | the engine | Path to `convert-safetensors.py`, when it is not found beside the binary. |
| `ONYX_BUILD_JOBS` | `npm run build` | Parallel compile jobs. The default is one per CPU. |
| `ONYX_SMOKE_PORT` | `scripts/smoke-test.sh` | Base port for the test instances (default `18190`). |

## Exit status

`0` after `--help` or `--version`, and when the server stops listening normally. `1` for an argument error, a refused `--host`, a model that will not resolve or load, a bind failure, or a server that terminates unexpectedly. A signal (`SIGTERM`, `SIGINT`) ends the process at once with the signal's usual status. See [supervising](../guides/supervising.md#exit-codes).
