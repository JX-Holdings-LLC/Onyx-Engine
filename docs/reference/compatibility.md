[Documentation](../README.md) › [Reference](README.md) › Compatibility and limits

# Compatibility and limits

Onyx Engine implements a subset of the OpenAI API and a few llama-server conveniences. It says no, with a `400` or `501` and a reason, rather than quietly ignoring what it cannot do. This page collects those refusals and the scope limits in one place, so you can tell "not supported" from "broken".

## What a request can expect

| You send | You get |
|---|---|
| `n` other than 1, on chat or completions | `400 only n=1 is supported` |
| `logit_bias`, `typical_p`, `mirostat`, `mirostat_tau`, `mirostat_eta`, `dynatemp_range`, `dynatemp_exponent`, `dry_multiplier`, `dry_base`, `dry_allowed_length`, `dry_penalty_last_n`, `xtc_probability`, `xtc_threshold` | `400 unsupported field: <name>` |
| `echo`, `suffix`, `best_of` on `/v1/completions` | `400 unsupported field: <name>` |
| `dimensions` on `/v1/embeddings` | `400 dimensions is not supported` |
| an `encoding_format` other than `float` | `400 only float encoding_format is supported` |
| `top_logprobs` without `logprobs: true`, or outside 0 to 20 | `400` |
| a prompt longer than one slot's context | `400 prompt does not fit in the context window` |
| a media URL (`http://`, `https://`, `file://`) | `400`; media must be inline base64 |
| `image_url` or `input_audio` without `--mmproj`, or to a projector without that modality | `400` |
| a `reasoning_budget_tokens` that cannot force tokens through a non-lazy grammar | `400` |
| a browser request (anything with an `Origin` header) | `403` |
| a missing or wrong bearer token, with `--api-key` set | `401` |
| a `Host` header that is not a loopback name, when bound to loopback | `403` |
| any route but `/health` while the model is loading | `503 Loading model` (`unavailable_error`) |
| a chat or text completion when `--max-queue` requests are already waiting | `503` with `Retry-After: 1` |
| a request body over 100 MiB | `413` |
| a route that the process's mode does not serve | `501`; see [which routes answer](api/README.md#which-routes-answer) |

`model` and `user` are accepted as client metadata and otherwise ignored: the process always uses the model it loaded.

## Scope

- **One model per process.** A second model is a second `onyx-engine` on another port. Multi-model routing is not planned; JX Runtime already does it.
- **Parallel slots divide the context.** `-np N` gives each request about `-c / N` tokens (`kv_unified` stays at llama.cpp's default of off). A slot that fills without `--context-shift` finishes with `finish_reason: "length"`.
- **A media prefill is a stall.** Encoding an image or audio clip runs inline at admission and holds the engine loop for that one prefill. See [multimodal input](../architecture/multimodal.md).
- **`--mmproj` turns off `--cache-reuse` and `--context-shift`**, process-wide, with a warning on stderr. A media chunk's KV positions cannot be partially discarded or prefix-matched token by token.
- **No I/O on a request's behalf.** The engine never fetches a URL or reads a file named in a request. Video is off by design (`MTMD_VIDEO=OFF` would shell out to `ffmpeg`).
- **The safetensors converter is narrow.** Hugging Face `LlamaForCausalLM`, optionally sharded, with a byte-level BPE `tokenizer.json`. Other architectures, SentencePiece `tokenizer.model` files, exotic dtypes, and tokenizer pre-processing or RoPE scaling it cannot represent are refused with a named reason.
- **Release binaries are CPU-only.** GPU backends are a build-time choice; see [building](../development/building.md#acceleration-option-matrix). The x64 archives require AVX2.
- **A decision model serves only `/v1/systemone`.** Image input works for four families and answers `501` for three; see [decision models](../guides/decision-models.md).
- **Speculative decoding is not implemented.** It is on the [roadmap](../../ROADMAP.md) as a decision still to make.

## Compared with llama-server

The route names, flag names and response shapes follow llama-server where onyx-engine implements the feature, which is why JX Runtime's adapter can drive either. The differences that matter:

| Area | llama-server | Onyx Engine |
|---|---|---|
| Web UI | yes | none; `--no-webui` is accepted and ignored |
| `/metrics` | behind `--metrics` | behind `--metrics`, with the same `llamacpp:*` names (a subset, plus `onyx:requests_rejected_total`) |
| Slot save and restore | yes | not yet; see the [roadmap](../../ROADMAP.md) |
| Readiness | listens at once, `503 Loading model` until loaded | the same, with the same body |
| `--port 0` | not documented | binds a free port and reports it on stderr and in `/props` |
| Queue limit | none | `--max-queue N`, answering `503` with `Retry-After` |
| Per-tick token budget | none (bounded by `--batch-size`) | `--tick-tokens N`, decode rows first |
| `X-Request-Id` | not echoed | echoed |
| Non-loopback `--host` without `--api-key` | allowed | refused unless `--allow-unauthenticated` |
| `Host` header on a loopback bind | not checked | must be a loopback name |
| API-key comparison | plain string compare | constant time |
| Sampler fields such as `mirostat`, `dry_*`, `xtc_*`, `logit_bias` | applied | refused with `400` |
| `/v1/models` | model metadata | exactly one entry, with `meta`; `created` is the request time |
| HTTP layer | its own build of `httplib` | a vendored `cpp-httplib` v0.54.1 compiled as `onyx-httplib` |
