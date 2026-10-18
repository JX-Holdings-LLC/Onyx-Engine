# Onyx Engine plan: LLM engine for Song Deck, template for the audio apps

Part of the [overall plan](./README.md). Repository:
[JX-Holdings-LLC/Onyx-Engine](https://github.com/JX-Holdings-LLC/Onyx-Engine) (0.4.0 when this was
written).

## Its two roles

1. **The LLM engine under JX Runtime.** Once Song Deck uses JX Runtime (milestone M1) with the
   `onyxengine` backend, Song Deck's language-model requests end up in Onyx Engine.
2. **The template for the audio apps.** The audio apps run on ggml, the tensor library under
   llama.cpp. Onyx Engine already solves pinned vendoring, CMake acceleration options (CUDA,
   Vulkan, Metal, ROCm, BLAS), a cpp-httplib server, a numpy-only safetensors→GGUF converter,
   and per-platform release builds.

What Onyx Engine should **not** become: a host for audio models. It's a one-model LLM server, and
its engine loop (tokens, request slots, the KV cache) has nothing to offer separation or
transcription. The audio apps get their own binaries.

## Where Onyx Engine stands today

- llama.cpp vendored as a pinned source tree (commit `9723942ad`), fetched by `scripts/vendor.sh`
  through a locally built `@jxburros/llama-cpp-source` package (not published yet).
- OpenAI-compatible chat, completions and embeddings; grammar, `json_schema` and
  `response_format` structured output; streaming; parallel slots.
- The safetensors converter handles only `LlamaForCausalLM` with a byte-level BPE tokenizer.
- CI and release workflows: `ci.yml`, `release.yml`, `publish-vendor.yml`.

## Work

### 1. Structured output for Song Deck (M1)

Song Deck relies on schema-constrained JSON for its composer, analysis and MIDI-editing roles
(`packages/ai/src/schemas/dialects.ts` in Song Deck).

- Run a set of Song Deck's real schemas through `json_schema` → GBNF: deep nesting, enums,
  `additionalProperties: false`, arrays of objects, `strict: true`.
- Check long outputs against `max_tokens` and context limits: a truncated JSON answer must fail
  clearly, not hand back half an object.
- Fix any schema feature the grammar conversion drops, and keep a couple of Song Deck schemas as a
  regression check in `scripts/smoke-test.sh`.
- **Done when:** Song Deck's LLM roles work through JX Runtime with the `onyxengine` backend as
  well as with `llamacpp`.

### 2. Write down the engine release convention (before M3)

JX Runtime downloads engines by release tag and asset name (`binaries.js`, `ONYX_RELEASES`). The
audio apps will feed the same installer.

- Write `docs/release-convention.md`: tag format, asset names per platform and accelerator,
  archive layout, a checksum file, and how a `--version` string maps to a tag.
- Onyx Engine follows it already, more or less. The audio apps adopt it, and JX Runtime's
  installer is written against the document, not against one repository.

### 3. Make the build scripts reusable (M2)

- Pick how the audio apps get ggml. Recommended: vendor standalone ggml
  (`ggml-org/ggml`) the same way Onyx Engine vendors llama.cpp, pinned to a ggml revision close to
  the one inside Onyx Engine's llama.cpp pin. The alternative (taking ggml out of the llama.cpp
  vendor package) ties the audio apps to llama.cpp's release pace for no gain.
- Parameterise `packaging/make-vendor-package.sh` and `scripts/vendor.sh` (package name, upstream
  repository, commit) so the audio-apps repository can reuse them with few changes.
- Optionally publish the vendor packages, so neither repository has to build them locally.

### 4. Optional: more safetensors architectures

Only needed if JX Runtime's safetensors installs route through Onyx Engine's converter for models
Song Deck users pick (Qwen, Gemma, Mistral families). GGUF downloads don't need it. Leave it unless
it blocks M6.

## Order and size

| Step                               | Milestone      | Size                              |
| ---------------------------------- | -------------- | --------------------------------- |
| 1. structured output for Song Deck | M1             | 3–5 days                          |
| 2. release convention              | before M3      | 1–2 days                          |
| 3. reusable build scripts          | M2             | 2–4 days                          |
| 4. more safetensors architectures  | only if needed | 1–2 weeks per architecture family |

## Notes

- This session had read-only access to Onyx Engine. The changes above are for a session with write
  access.
- Keep the llama.cpp pin and the audio apps' ggml pin moving together where practical, so a ggml
  fix or a new GPU backend reaches both.
