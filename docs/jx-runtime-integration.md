# JX Runtime Integration

**Status: no adapter exists yet.** JX Runtime's current llama.cpp backend
(`src/backends/llamacpp.js` in the JX-Runtime repository) spawns and speaks
to upstream `llama-server`, not `jx-engine`. This document maps what that
adapter already does against upstream `llama-server` onto what `jx-engine`
actually provides, as a contract for a *future* `jxengine` adapter — it does
not describe integration that exists today. Treat every "the adapter does
X" statement below as "JX Runtime's llamacpp adapter does X against
`llama-server` today, and `jx-engine` is built to answer the same way."

## Endpoints the adapter calls

JX Runtime's `LlamaCppAdapter` (in `src/backends/llamacpp.js`) talks to its
child engine process over a small, fixed set of endpoints. `jx-engine`
implements all of them (see [`api.md`](api.md) for exact request/response
shapes):

- **Health polling.** `_waitReady()` polls `GET /health` with exponential
  backoff (starting at 25ms, capped at 300ms) until it gets a response,
  treating that as "the process is up." `jx-engine`'s `/health` always
  returns `{"status":"ok"}` once the server has bound its port — which is
  after `jx_engine::load()` has already finished loading the model, so a
  `jx-engine` health check that succeeds means the model is loaded, not just
  that the process has started.
- **`/props` capture at load time.** `_captureProps()` calls `GET /props`
  once, right after readiness, and records `chat_template` (as a
  boolean-present + truncated SHA-256, never the template text itself),
  `build_info`, and a context-length figure it reads defensively from either
  a top-level `n_ctx` or a nested `default_generation_settings.n_ctx` (to
  cover both old and new `llama-server` response shapes). `jx-engine`'s
  `/props` sets **both** `n_ctx` at the top level and
  `default_generation_settings.n_ctx` nested, to the same value — so either
  read path the adapter already has finds the real context size.
- **`/tokenize` for preflight.** `tokenCount()` calls `POST /tokenize` with
  `{"content": ...}` and reads `data.tokens.length`. `jx-engine`'s
  `/tokenize` accepts exactly that body shape and returns
  `{"tokens": [...]}`.
- **`/apply-template` for prompt rendering / opt-in capture.**
  `renderPrompt()` calls `POST /apply-template` with a chat-completions-style
  body and reads `data.prompt`. `jx-engine`'s `/apply-template` returns
  `{"prompt": "..."}` from the same kind of body.
- **Inference proxying (`KIND_PATHS`).** JX Runtime's `KIND_PATHS` maps its
  own request kinds to the paths it proxies to the child engine:
  `chat` → `/v1/chat/completions`, `completion` → `/v1/completions`,
  `embeddings` → `/v1/embeddings`. `jx-engine` implements all three exactly
  where the adapter expects them.

All of these calls (except the streaming inference proxy itself) go through
`_engineJson()`, a small bounded best-effort JSON fetch with a timeout —
any non-200, timeout, or unparseable response is treated as "the engine
didn't say" (`null`), never as a hard error. This means a `jx-engine` build
that omits or changes one of these endpoints degrades gracefully in the
adapter rather than failing loads outright — but the value of that endpoint
(e.g. the recorded chat-template hash, or preflight token counts) is simply
lost.

## `--alias` as the model identity

JX Runtime's `buildArgs()` passes `--alias <registry model id>` to
`llama-server` specifically so the engine's OpenAI-compatible responses echo
back the runtime's own human-readable model id, rather than the on-disk
blob path passed via `-m`. `jx-engine`'s `--alias` does exactly this: it
sets `jx_engine::alias_`, which is what `/v1/models`, `/v1/chat/completions`,
`/v1/completions`, and `/v1/embeddings` all report as `model`/`id`. If
`--alias` is not passed, `jx-engine` derives one from the model filename
(stripping directory and `.gguf` extension) — an adapter integration should
always pass `--alias` explicitly rather than rely on that derivation, to
guarantee its own model id is what comes back.

## `--version`/`--help` CLI probing

JX Runtime's adapter never assumes a capability; it probes for it:

- `detectSupport()` runs `<binary> --version` (5s timeout) to confirm the
  binary starts at all and to read a version string, and specifically
  distinguishes "could not spawn" / "loader/library failure" from "answered
  but exited non-zero" (the latter is treated leniently since some
  `llama-server` builds exit non-zero on `--version`). `jx-engine
  --version` prints `jx-engine <version>` then
  `llama.cpp build <LLAMA_BUILD_NUMBER> (<LLAMA_COMMIT>)` and exits `0` —
  a clean answer either probe style would accept.
- `supportsJinja()`, `supportsMmproj()`, and the generic `supportsFlag(flag)`
  all run `<binary> --help` once (cached per binary) and regex-search the
  output for a flag name before ever passing that flag on a real launch —
  specifically because an older `llama-server` build exits during argument
  parsing on an unrecognized flag, which would otherwise turn "we tried to
  use a newer feature" into "the model failed to load."

This is exactly why `jx-engine`'s `--help` output is curated rather than
exhaustive (see `src/args.h`'s header comment): **flags `jx-engine` does not
implement — `--mmproj`, `--reasoning-budget` — are
deliberately absent from both the argument parser and the `--help` text.**
An adapter's `supportsFlag('--mmproj')`/
`supportsFlag('--reasoning-budget')` probe against `jx-engine --help` will
correctly come back `false`, and the adapter's existing "don't pass a flag
the build didn't advertise" logic then simply never sends them — with no
special-casing needed for `jx-engine` versus `llama-server`. This is a
property to preserve, not incidental: a future `jx-engine` version that adds
one of these flags should add it to `--help` and the parser together, so the
same probe mechanism picks it up automatically.

Two things the current adapter probes for that this mapping calls out
explicitly because they matter to correctness, not just capability:

- **`--flash-attn` value vs. toggle form.** The adapter's `flashAttnStyle()`
  probe distinguishes `-fa on|off|auto` (value form) from a bare `-fa`
  toggle by pattern-matching the `--help` line. `jx-engine`'s `-fa`/
  `--flash-attn` only accepts the value form (`on`/`off`/`auto` — see
  `src/args.cpp`); its `--help` text lists `on, off, auto` explicitly, which
  the adapter's existing value-form regex (`/on\|off\|auto|'on'/`) already
  matches correctly.
- **`--parallel` is always passed, unconditionally, by the adapter** (see
  the long comment in `buildArgs()`), on the premise that an engine handed
  no `--parallel` at all might default to more than one slot — the adapter
  states the slot count it decided on every launch rather than relying on
  the engine's own default. As of v2 `jx-engine` honours it: `--parallel N`
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

`jx-engine`'s final SSE frame for both `/v1/chat/completions` and
`/v1/completions` carries exactly the fields this observer looks for: a
top-level `usage` object with `prompt_tokens`/`completion_tokens`/
`total_tokens`, and a top-level `timings` object with `prompt_ms`/
`predicted_ms`/`prompt_per_second`/`predicted_per_second` (plus `cache_n`/
`prompt_n`/`predicted_n`, which the observer does not currently read but
does not choke on either — it only looks for the keys it wants). The
observer will register `_reported` (exact counts, `estimated: false`) rather
than falling back to per-frame estimation against a real `jx-engine`
stream, and will pick up `finish_reason` and the `timings` breakdown from
`jx-engine`'s finish/usage frames the same way it does from `llama-server`'s.

## Adding a `jxengine` adapter (future work — not implemented)

Nothing below exists in JX Runtime today; this is the shape a future
integration would plausibly take, based on how the existing `llamacpp`
adapter is structured, for a reader deciding whether/how to build it:

- An `execution.backend` enum value (alongside whatever values JX Runtime
  already supports there) naming `jxengine`, selecting a new adapter class
  implementing the same `BackendAdapter` interface `LlamaCppAdapter` does.
- A `backends.jxengine` config section, analogous to `backends.llamacpp`
  (`serverPath`/`autoManage`, timeouts, jinja/cache-reuse-style toggles),
  though a `jxengine` adapter would need less of it than the `llamacpp`
  adapter carries today — `jx-engine` has no separate `--jinja` toggle to
  reason about (jinja is always on) and no `--mmproj`/`--reasoning-budget`
  capability gating to do at all, since those flags are simply absent.
- The `buildArgs()`-equivalent for `jxengine` would be substantially
  simpler than the current `llamacpp` one: no flash-attn-style probing
  needed if `jx-engine`'s value-only `-fa` form is assumed, and
  `--parallel`/`--context-shift` can be passed on the same terms as to
  `llama-server`, since v2 implements both.

This section is intentionally scoped to "what would this look like," not an
implementation plan — building it is out of scope for this document and for
`jx-engine` itself.
