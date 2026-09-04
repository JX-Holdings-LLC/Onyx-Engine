# JX Runtime Integration

**Status: JX Runtime has an `onyxengine` adapter.** JX Runtime's llama.cpp
backend (`src/backends/llamacpp.js` in the JX-Runtime repository) spawns and
speaks to upstream `llama-server`; alongside it, JX Runtime now ships a
separate `onyxengine` adapter (`src/backends/onyxengine.js`), selected via
`execution.backend: 'onyxengine'` and configured under
`backends.onyxengine.*`, that spawns `onyx-engine` the same way the llamacpp
adapter spawns `llama-server`. A managed install path
(`jx-runtime backend install --engine onyx`) downloads a prebuilt
`onyx-engine` from this repo's GitHub Releases (see
[`README.md#prebuilt-binaries`](../README.md#prebuilt-binaries)) into
`<data-dir>/engines/onyx-engine/`, pinned to a specific release tag and
asset-name scheme — see `CHANGELOG.md` for which tag. This document maps
what the llamacpp adapter does against upstream `llama-server` onto what
`onyx-engine` actually provides; it describes the contract the `onyxengine`
adapter is built against, not a line-by-line account of that adapter's own
source, which this repository does not contain. Treat every "the adapter
does X" statement below as "JX Runtime's llamacpp adapter does X against
`llama-server` today, and `onyx-engine` is built to answer the same way" —
the `onyxengine` adapter's own behavior lives in the JX-Runtime repository.

## Endpoints the adapter calls

JX Runtime's `LlamaCppAdapter` (in `src/backends/llamacpp.js`) talks to its
child engine process over a small, fixed set of endpoints. `onyx-engine`
implements all of them (see [`api.md`](api.md) for exact request/response
shapes):

- **Health polling.** `_waitReady()` polls `GET /health` with exponential
  backoff (starting at 25ms, capped at 300ms) until it gets a response,
  treating that as "the process is up." `onyx-engine`'s `/health` always
  returns `{"status":"ok"}` once the server has bound its port — which is
  after `onyx_engine::load()` has already finished loading the model, so an
  `onyx-engine` health check that succeeds means the model is loaded, not just
  that the process has started.
- **`/props` capture at load time.** `_captureProps()` calls `GET /props`
  once, right after readiness, and records `chat_template` (as a
  boolean-present + truncated SHA-256, never the template text itself),
  `build_info`, and a context-length figure it reads defensively from either
  a top-level `n_ctx` or a nested `default_generation_settings.n_ctx` (to
  cover both old and new `llama-server` response shapes). `onyx-engine`'s
  `/props` sets **both** `n_ctx` at the top level and
  `default_generation_settings.n_ctx` nested, to the same value — so either
  read path the adapter already has finds the real context size.
- **`/tokenize` for preflight.** `tokenCount()` calls `POST /tokenize` with
  `{"content": ...}` and reads `data.tokens.length`. `onyx-engine`'s
  `/tokenize` accepts exactly that body shape and returns
  `{"tokens": [...]}`.
- **`/apply-template` for prompt rendering / opt-in capture.**
  `renderPrompt()` calls `POST /apply-template` with a chat-completions-style
  body and reads `data.prompt`. `onyx-engine`'s `/apply-template` returns
  `{"prompt": "..."}` from the same kind of body.
- **Inference proxying (`KIND_PATHS`).** JX Runtime's `KIND_PATHS` maps its
  own request kinds to the paths it proxies to the child engine:
  `chat` → `/v1/chat/completions`, `completion` → `/v1/completions`,
  `embeddings` → `/v1/embeddings`. `onyx-engine` implements all three exactly
  where the adapter expects them.

All of these calls (except the streaming inference proxy itself) go through
`_engineJson()`, a small bounded best-effort JSON fetch with a timeout —
any non-200, timeout, or unparseable response is treated as "the engine
didn't say" (`null`), never as a hard error. This means an `onyx-engine` build
that omits or changes one of these endpoints degrades gracefully in the
adapter rather than failing loads outright — but the value of that endpoint
(e.g. the recorded chat-template hash, or preflight token counts) is simply
lost.

## `--alias` as the model identity

JX Runtime's `buildArgs()` passes `--alias <registry model id>` to
`llama-server` specifically so the engine's OpenAI-compatible responses echo
back the runtime's own human-readable model id, rather than the on-disk
blob path passed via `-m`. `onyx-engine`'s `--alias` does exactly this: it
sets `onyx_engine::alias_`, which is what `/v1/models`, `/v1/chat/completions`,
`/v1/completions`, and `/v1/embeddings` all report as `model`/`id`. If
`--alias` is not passed, `onyx-engine` derives one from the model filename
(stripping directory and `.gguf` extension) — an adapter integration should
always pass `--alias` explicitly rather than rely on that derivation, to
guarantee its own model id is what comes back.

## `--version`/`--help` CLI probing

JX Runtime's adapter never assumes a capability; it probes for it:

- `detectSupport()` runs `<binary> --version` (5s timeout) to confirm the
  binary starts at all and to read a version string, and specifically
  distinguishes "could not spawn" / "loader/library failure" from "answered
  but exited non-zero" (the latter is treated leniently since some
  `llama-server` builds exit non-zero on `--version`). `onyx-engine
  --version` prints `onyx-engine <version>` then
  `llama.cpp build <LLAMA_BUILD_NUMBER> (<LLAMA_COMMIT>)` and exits `0` —
  a clean answer either probe style would accept.
- `supportsJinja()`, `supportsMmproj()`, and the generic `supportsFlag(flag)`
  all run `<binary> --help` once (cached per binary) and regex-search the
  output for a flag name before ever passing that flag on a real launch —
  specifically because an older `llama-server` build exits during argument
  parsing on an unrecognized flag, which would otherwise turn "we tried to
  use a newer feature" into "the model failed to load."

This is exactly why `onyx-engine`'s `--help` output is curated rather than
generated: **the parser and `--help` move together, always** (see
`src/args.h`'s header comment) — a flag never appears in one without the
other. As of v2, `--mmproj`, `--context-shift`, `--reasoning-budget`, and
`--parallel` are all real, both accepted by the parser and listed in
`--help`. An adapter's `supportsFlag('--mmproj')`,
`supportsFlag('--context-shift')`, and `supportsFlag('--reasoning-budget')`
probes against `onyx-engine --help` now correctly come back `true`, and the
adapter's existing "only pass a flag the build advertised" logic then simply
starts sending them — no special-casing needed for `onyx-engine` versus
`llama-server`, and no adapter code change required to pick this up. What
passing each one now buys:

- **`--mmproj PATH`** enables multimodal chat: `image_url`/`input_audio`
  content parts become acceptable in `/v1/chat/completions` requests (as
  `data:`/base64 payloads only — see [`api.md`](api.md)), and `GET /props`'s
  `modalities` starts reporting real vision/audio support instead of
  `false`/`false`. It also force-disables `--cache-reuse` and
  `--context-shift` process-wide (a media chunk's KV can't be
  prefix-matched or partially discarded) — an adapter that raises both
  flags together on a multimodal launch will see the latter two silently
  overridden, with a warning on `onyx-engine`'s stderr, not a launch failure.
- **`--context-shift`** lets a slot that fills its context keep generating
  by dropping its oldest non-preserved tokens instead of stopping at
  `n_predict`/context limit — useful for adapters that would rather trade
  early context for a completed response than hit `finish_reason: "length"`
  early. `--keep N` controls how much of the front of the prompt survives a
  shift.
- **`--reasoning-budget N`** (plus `--reasoning-budget-message` and the
  per-request `reasoning_budget_tokens` override) caps or suppresses a
  thinking model's `<think>...</think>` block — an adapter fronting a
  reasoning model can bound latency/token spend on the thinking phase
  without needing model-specific prompt engineering.

This remains a property to preserve going forward, not incidental: any
future `onyx-engine` flag should be added to `--help` and the parser together,
so the same probe mechanism picks it up automatically without an adapter
update.

Two things the current adapter probes for that this mapping calls out
explicitly because they matter to correctness, not just capability:

- **`--flash-attn` value vs. toggle form.** The adapter's `flashAttnStyle()`
  probe distinguishes `-fa on|off|auto` (value form) from a bare `-fa`
  toggle by pattern-matching the `--help` line. `onyx-engine`'s `-fa`/
  `--flash-attn` only accepts the value form (`on`/`off`/`auto` — see
  `src/args.cpp`); its `--help` text lists `on, off, auto` explicitly, which
  the adapter's existing value-form regex (`/on\|off\|auto|'on'/`) already
  matches correctly.
- **`--parallel` is always passed, unconditionally, by the adapter** (see
  the long comment in `buildArgs()`), on the premise that an engine handed
  no `--parallel` at all might default to more than one slot — the adapter
  states the slot count it decided on every launch rather than relying on
  the engine's own default. As of v2 `onyx-engine` honours it: `--parallel N`
  allocates `N` real request slots served by one continuous-batching engine
  loop (see [`architecture.md`](architecture.md)), and the default remains
  `1`. Note that `N` also divides the context — with `kv_unified` left at
  llama.cpp's default, each slot gets roughly `n_ctx / N` tokens — so an
  adapter that raises `--parallel` should raise `-c` to match, or accept
  smaller per-request context windows.

## SSE usage-frame accounting (`sseUsage.js`)

JX Runtime's `StreamUsageObserver` (`src/sseUsage.js`) is a passive,
read-only tap on the raw SSE byte stream being forwarded to the client. It
looks for `data:` lines whose JSON contains a `usage`, `timings`, or
non-null `finish_reason` key, and when found:

- reads `usage.prompt_tokens` / `usage.completion_tokens` into its
  `_reported` counts (superseding any earlier frame — "a later usage frame
  supersedes an earlier one: engines send the authoritative totals last"),
- reads `choices[0].finish_reason` into `_finishReason`,
- reads `timings.prompt_ms` / `timings.predicted_ms` /
  `timings.prompt_per_second` / `timings.predicted_per_second` into
  `_timings`.

If no `usage` frame ever arrives, it falls back to counting SSE frames that
carried non-empty `content`/`text` (one frame ≈ one token, `estimated: true`
in its result).

`onyx-engine`'s final SSE frame for both `/v1/chat/completions` and
`/v1/completions` carries exactly the fields this observer looks for: a
top-level `usage` object with `prompt_tokens`/`completion_tokens`/
`total_tokens`, and a top-level `timings` object with `prompt_ms`/
`predicted_ms`/`prompt_per_second`/`predicted_per_second` (plus `cache_n`/
`prompt_n`/`predicted_n`, which the observer does not currently read but
does not choke on either — it only looks for the keys it wants). The
observer will register `_reported` (exact counts, `estimated: false`) rather
than falling back to per-frame estimation against a real `onyx-engine`
stream, and will pick up `finish_reason` and the `timings` breakdown from
`onyx-engine`'s finish/usage frames the same way it does from `llama-server`'s.

## The `onyxengine` adapter

JX Runtime ships a `onyxengine` adapter alongside its `llamacpp` one:

- An `execution.backend: 'onyxengine'` value selects a `src/backends/onyxengine.js`
  adapter class implementing the same `BackendAdapter` interface
  `LlamaCppAdapter` does.
- A `backends.onyxengine` config section, analogous to `backends.llamacpp`
  (`serverPath`/`autoManage`, timeouts, jinja/cache-reuse-style toggles),
  needs less of it than the `llamacpp` adapter carries — `onyx-engine` has no
  separate `--jinja` toggle to reason about (jinja is always on). It still
  gates `--mmproj`/`--reasoning-budget` via `supportsFlag`, same as any other
  flag, since those remain optional features a given launch may or may not
  want, even though both are real as of `onyx-engine` v2.
- Its `buildArgs()`-equivalent is substantially simpler than the `llamacpp`
  one: no flash-attn-style probing needed given `onyx-engine`'s value-only
  `-fa` form, and `--parallel`/`--context-shift`/`--mmproj`/
  `--reasoning-budget` are all passed on the same terms as to `llama-server`,
  since v2 implements all of them. It does *not* pass `--no-webui`: the
  `onyxengine` adapter strips that flag from the shared launch arguments,
  because v0.2.0 rejects it and v0.3.0 only accepts it as a no-op (see
  `src/args.h`/`src/args.cpp`), so there is no build on which passing it
  buys anything. Accepting it keeps a hand-configured launch that reuses
  `llama-server` arguments from failing at argument parsing.
- A managed install path (`jx-runtime backend install --engine onyx`)
  downloads a prebuilt `onyx-engine` binary from this repo's GitHub
  Releases (see [`README.md#prebuilt-binaries`](../README.md#prebuilt-binaries)
  and [`docs/building.md#release-binaries`](building.md#release-binaries) for
  the artifact naming scheme) into `<data-dir>/engines/onyx-engine/`, the
  same way JX Runtime already manages its pinned `llama-server` download.

This section describes the contract the `onyxengine` adapter is built
against from the `onyx-engine` side; the adapter's own implementation lives
in the JX-Runtime repository, not here.
