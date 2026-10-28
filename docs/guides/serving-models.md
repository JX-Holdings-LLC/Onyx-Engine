[Documentation](../README.md) › [Guides](README.md) › Serving models

# Serving models

One `onyx-engine` process serves one model. This guide covers choosing the model file, sizing the context and the slots, using a GPU, and the flags that change behavior for a whole process.

## The model file

| You have | Pass | What happens |
|---|---|---|
| A GGUF file | `-m model.gguf` | Loaded as it is. Any `.gguf` extension, or any file that starts with the `GGUF` magic bytes, passes through untouched. |
| A Hugging Face directory | `-m ./Llama-3-8B-hf/` | Converted to an f16 GGUF on first load, cached, and reused. |
| A `.safetensors` file in such a directory | `-m ./Llama-3-8B-hf/model.safetensors` | Same as above; the parent directory supplies the config and tokenizer. |

Prefer a pre-quantized GGUF. The converter writes f16 only, which is large, and it handles only the standard `LlamaForCausalLM` layout with a byte-level BPE `tokenizer.json`. Anything else is refused with a reason rather than mishandled. [How resolution works](../architecture/safetensors-conversion.md) has the cache rules; to keep converted files out of the model directory, pass `--convert-dir`.

## Name it

```bash
onyx-engine -m ./models/llama-3-8b-q4.gguf --alias llama-3-8b
```

`--alias` is the model id that `/v1/models`, `/v1/chat/completions` and the rest report. If you leave it out the engine derives one from the file name. Anything that routes by model id should pass it explicitly.

## Context and slots

`-c` is the context in tokens: `-c 0` takes the model's training context, and the default is 4096. `-np N` creates `N` request slots that generate at the same time. The context is **divided** among the slots, so each request gets about `-c / N` tokens:

![Eight thousand tokens of context split four ways gives four slots of 2048](../../assets/diagrams/svg/context-slots.svg)

```bash
# four requests at once, each with a 4096-token window
onyx-engine -m model.gguf -c 16384 -np 4
```

Rules of thumb:

- Decide the largest prompt plus answer you will allow one request, multiply by the number of simultaneous requests, and pass that as `-c`.
- A prompt that does not fit one slot is refused with a `400` (`prompt does not fit in the context window`). Read the per-slot window from `GET /props` as `n_ctx`; `n_ctx_total` is the undivided figure and is informational.
- With one slot nothing is divided, and requests queue first in, first out.
- A longer context costs memory in proportion. If a model will not load, lower `-c` before anything else.

### When a slot fills mid-generation

By default the request finishes with `finish_reason: "length"`. With `--context-shift` the slot drops the oldest half of its unprotected tokens and keeps generating, and the response carries `truncated: true`. `--keep N` protects the first `N` prompt tokens (`-1` protects the whole prompt), which you want for a system prompt. Shifting is switched off when the model's memory cannot shift (some sliding-window models) and whenever `--mmproj` is loaded. See [KV cache](../architecture/kv-cache.md).

### Repeated prefixes

Each slot remembers the tokens it last decoded. A new request that starts with the same tokens (a growing chat, or one system prompt across many calls) decodes only the part that differs. The response's `timings.cache_n` says how many tokens were reused. `--cache-reuse N` sets the minimum prefix worth reusing; `0` turns it off. Sliding-window models (Gemma, gpt-oss) reuse prefixes like dense ones; recurrent and hybrid models (Mamba, Granite 4, Qwen3.5 hybrids) reuse the cache when a new prompt extends the previous one exactly, as the next chat turn does.

## GPUs

Release binaries are CPU-only. For a GPU, build with one backend and offload layers at run time:

```bash
cmake -B build -DONYX_ENGINE_CUDA=ON
cmake --build build --target onyx-engine -j 8
./build/onyx-engine -m model.gguf -ngl 99
```

`-ngl -1` (the default) offloads every layer that fits; a number offloads that many; `0` stays on the CPU. Partial offload runs the rest of the layers on the CPU. See [building](../development/building.md#acceleration-option-matrix) for each backend's toolchain. `-fa auto` lets llama.cpp pick flash attention; force it with `-fa on` or `-fa off`.

## Threads and memory

`-t` sets generation threads and `-tb` prompt-processing threads; leave both at `-1` unless you have measured. `--mlock` pins the weights in RAM so the OS cannot page them out, and `--no-mmap` reads the file into memory instead of mapping it.

### Fitting a bigger model

Two kinds of memory decide what fits: the KV cache, which grows with `-c`, and the weights.

- **The KV cache.** `-ctk q8_0 -ctv q8_0` stores it at 8 bits, about half of the default `f16`, with very little quality loss on most models. A quantized V cache needs flash attention, which `-fa auto` turns on. Treat the 4-bit types (`q4_0`, `iq4_nl`, ...) with care: some models, sliding-window ones especially, lose a lot of quality with them.
- **MoE experts.** A Mixture-of-Experts model reads only a few experts per token, so keeping the expert tensors in system RAM and the rest on the GPU costs much less speed than offloading whole layers. `--cpu-moe` keeps them all on the CPU; `--n-cpu-moe N` keeps only the first `N` layers' experts there, so raise `N` until the model fits. `-ot PATTERN=BUFFER` places any tensor by name for finer control, for example `-ot 'blk\.(1[0-9])\.ffn_.*_exps=CPU'`.

```bash
./build/onyx-engine -m qwen3-30b-a3b-q4_k_m.gguf -ngl 99 --n-cpu-moe 24 -ctk q8_0 -ctv q8_0 -c 32768
```

## Chat templates

The model's own Jinja template is used, and Jinja is always on. A model with no template falls back to ChatML. Override it with `--chat-template-file path` (or `--chat-template` with Jinja source, or the name `chatml`) when a model ships a wrong template. Check what is in force with `GET /props` (`chat_template`) and see the rendered prompt for any messages with `POST /apply-template`.

## Reasoning models

`--reasoning-budget N` bounds a thinking model's `<think>` block: `0` suppresses it, `N` allows `N` tokens inside it and then forces it closed, `-1` leaves it alone. `--reasoning-budget-message` supplies text to inject before the forced closing tag. A request can override the budget with `reasoning_budget_tokens`. Details: [reasoning budget](../architecture/reasoning-and-logprobs.md).

## A launch you can start from

```bash
onyx-engine \
  -m ./models/model-q4.gguf --alias assistant \
  --port 8081 --api-key "$ONYX_KEY" \
  -c 16384 -np 4 --context-shift --keep 256 \
  -ngl -1
```

The same flags, with the reasons, are in the [command-line reference](../reference/cli.md).
