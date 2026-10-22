[Documentation](../README.md) › [Reference](README.md) › Stability

# Stability

What you can build on, and what can change under you. This page covers every program that drives
`onyx-engine`. JX Runtime gets no private contract. It relies only on behaviour listed here as
stable, the same as anyone else.

## Versions and how breaking changes are announced

Onyx Engine is pre-1.0. Every change to `main` advances the version: a patch by default (the
`every-main-change` policy in [releasing](../development/releasing.md)).

- **A patch release (`0.6.1` → `0.6.2`) never breaks anything marked stable below.** It can add
  flags, routes, response fields and metrics.
- **A change that breaks a stable item advances the minor version** (`0.6.x` → `0.7.0`) and is
  listed under a **Breaking** heading in the [changelog](../../CHANGELOG.md) and the release
  notes. The heading says what changed, why, and what a caller has to do.
- After 1.0, the same rule moves up one place: breaking changes need a new major version.

"Stable" means it will not be removed, renamed or given a different meaning, type or default
without that announcement. It does not mean it will never change.

## Stable

| Area | What is stable |
|---|---|
| **Command line** | Every flag listed in `--help`, with its spelling, aliases, value form and default. **In `--help` means accepted:** the parser and the help text change together, so a supervisor can probe `--help` for a flag name before passing it. A flag not in `--help` is never accepted. The `-fa` help line keeps the `on\|off\|auto` form that probes match on. |
| **`--version`** | Two lines: `onyx-engine <version>`, then `llama.cpp <build>-<commit>`, exit status 0. |
| **Routes** | The method and path of every route in the [API reference](api/README.md), and which process mode serves it (`501` outside it). |
| **Request fields** | Every documented field keeps its meaning. A field Onyx refuses today (`400`, listed in [compatibility](compatibility.md)) may become accepted later; an accepted field never becomes refused without a breaking release. |
| **Response shapes** | Documented fields keep their names, types and meaning. New fields can appear in any release, so clients must ignore fields they do not know. |
| **Errors** | The envelope `{"error":{"message","type","code"}}` and the status code for each condition. `type` is one of `invalid_request_error`, `authentication_error`, `not_supported_error`, `unavailable_error` or `server_error`. |
| **`/health`** | Exempt from `--api-key`. `503` with `{"error":{"code":503,"message":"Loading model","type":"unavailable_error"}}` from the moment the port opens until the model is loaded; `200 {"status":"ok"}` after that. A refused connection means the process is not running. |
| **Readiness of other routes** | Every other route answers `503` with the same body while the model loads (after any API-key check). |
| **Streaming** | `data: {...}` frames, then a final frame with top-level `usage` (`prompt_tokens`, `completion_tokens`, `total_tokens`) and `timings` (`cache_n`, `prompt_n`, `prompt_ms`, `prompt_per_second`, `predicted_n`, `predicted_ms`, `predicted_per_second`) and an empty `choices` for chat, then `data: [DONE]`. Non-streaming responses carry the same `usage` and `timings` objects. |
| **`/props`** | The fields documented in [service endpoints](api/service-endpoints.md#get-props), including the per-slot `n_ctx` (equal to `default_generation_settings.n_ctx`), `n_ctx_total` and `port`. |
| **Exit codes** | `0` after `--help` or `--version`. `1` for an invalid argument, a refused `--host`, a failed bind, a failed conversion or a failed load; each prints a line starting `error:` on stderr first. A signal ends the process with the signal's usual status. |
| **Two stderr lines** | `onyx-engine listening on http://HOST:PORT (loading)` once the port is open, and `onyx-engine ready on http://HOST:PORT (...)` once the model is loaded. Supervisors can parse the port from the first line (useful with `--port 0`). |
| **Operations** | The `X-Request-Id` echo rule, `--max-queue`'s `503` with `Retry-After`, and the `/metrics` names listed in [service endpoints](api/service-endpoints.md#get-metrics). |
| **Security defaults** | Binding `127.0.0.1` by default; refusing a non-loopback `--host` without `--api-key` (or `--allow-unauthenticated`); refusing requests that carry `Origin`; refusing a non-loopback `Host` header on a loopback bind. A future release may tighten these further; that counts as breaking and is announced. |
| **Release assets** | The archive names `onyx-engine-v<version>-<platform>-<arch>.<ext>` for the platforms listed in [releasing](../development/releasing.md#what-gets-published), flat archive layout, and `checksums.txt`. New platforms or variants can be added; existing names are not removed in a patch release. |
| **Limits** | Request bodies up to 100 MiB (larger ones get `413`); `X-Request-Id` values up to 128 printable ASCII characters. Raising a limit is not breaking; lowering one is. |

## Not stable

These can change in any release, with a changelog line but no breaking announcement:

- **Error message text.** Branch on the status code and `error.type`, not on `error.message`.
- **Other stderr output:** the loading and model-loaded lines, warnings, llama.cpp's own logging,
  and anything printed with `--verbose`.
- **The llama.cpp pin.** It moves when upstream fixes or features are worth taking, in a patch
  release if nothing stable changes. Model support follows upstream, and generated text and
  logprobs can differ slightly across pins for the same seed.
- **Performance:** speed, memory use and scheduling details, including how `--tick-tokens` splits
  a tick.
- **`--help` layout.** Wording, ordering and alignment can change; the flag names in it cannot.
- **Undocumented behaviour,** including response fields that appear in the output but not in the
  [API reference](api/README.md).
- **Build options and scripts** under `scripts/`, `packaging/` and `cmake/`, and the CMake
  presets. These are covered by the [development docs](../development/README.md), not by this
  page.

## For integrators

- Probe `--help` for a flag before passing it, and `--version` for the build. That works across
  versions without knowing which release added what.
- Treat `503` from `/health` as "keep waiting", `200` as ready, and a refused connection or a
  process exit as failure. The [supervisor guide](../guides/supervising.md) has worked examples.
- Ignore unknown JSON fields and unknown `/metrics` series.
- Read the changelog's **Breaking** headings before moving to a new minor version.

JX Runtime's adapter follows these rules; the [JX Runtime integration](../integration/jx-runtime.md)
page lists what it uses, and every item there is on this page.
