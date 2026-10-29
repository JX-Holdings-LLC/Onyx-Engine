# cpp-httplib (vendored)

`httplib.h` / `httplib.cpp` from [yhirose/cpp-httplib], version **0.62.0**,
copied verbatim. `LICENSE` is that project's (MIT).

Upstream ships one header; the two files here are its own `split.py -e cpp`
output for the `v0.62.0` tag (the 0.54.1 copy was checked the same way and
matched byte for byte). The pinned llama.cpp (b11820) vendors the same
version in `vendor/cpp-httplib`, so the copy inside `llama-common` and this
one agree.

## Why this is here

`src/server.cpp` is built on cpp-httplib. It used to reach into the vendored
llama.cpp source tree for it — an include path pointing at
`<llama src>/vendor/cpp-httplib` — while the *implementation* came from
`libllama-common.so`, which has upstream's own cpp-httplib statically linked
into it and re-exports ~1200 `httplib::*` symbols. Nothing about onyx-engine's
HTTP layer is llama.cpp's concern, and that arrangement had two problems:

1. onyx-engine's HTTP server ran on code it got out of llama.cpp's shared
   library, as a private implementation detail of `llama-common`. Any
   upstream change — moving the vendored copy, building `common` with hidden
   visibility, bumping cpp-httplib, dropping it — breaks onyx-engine's server,
   or silently binds it to a different cpp-httplib than the header it was
   compiled against.
2. Upstream sets cpp-httplib's tuning macros `PRIVATE` to *its* target, so
   they applied when `httplib.cpp` was compiled but **not** when onyx-engine
   compiled `server.cpp` against the same header. One of them is used from
   the header: `CPPHTTPLIB_TCP_NODELAY` is the default member initializer of
   `httplib::Server::tcp_nodelay_`. The two translation units therefore
   disagreed on a class definition — an ODR violation.

   To be precise about the impact: `Server::Server()` is defined out of line
   in `httplib.cpp`, and no inline code in the header reads `tcp_nodelay_`,
   so the value that actually took effect was upstream's (`1`) and Nagle was
   in fact disabled. The mismatch was latent, not live — it would have become
   a real behavior difference the moment upstream used one of these macros
   from header-inline code.

Owning the copy fixes both: the macros below are `PUBLIC` on our target, so
the header and the implementation are always compiled with identical values.

`CMakeLists.txt` also lists `onyx-httplib` **before** `llama-common` on
onyx-engine's link line. That ordering is load-bearing: with `llama-common`
first, 15 httplib symbols still resolved to its shared library, leaving the
HTTP layer split across two copies of cpp-httplib. Our archive first means
`nm -u build/onyx-engine | grep httplib` is empty.

## Local build settings

Set in `CMakeLists.txt` (`onyx-httplib` target), matching the values llama.cpp
chose for the same workload:

| macro | value | why |
| --- | --- | --- |
| `CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH` | `1048576` | larger prompts in form payloads |
| `CPPHTTPLIB_LISTEN_BACKLOG` | `512` | avoid connection resets with many slots |
| `CPPHTTPLIB_REQUEST_URI_MAX_LENGTH` | `32768` | longer prompts in a query string |
| `CPPHTTPLIB_TCP_NODELAY` | `1` | disable Nagle — SSE frames must not be delayed |

TLS is deliberately not enabled: onyx-engine binds `127.0.0.1` and is fronted
by JX Runtime, so cpp-httplib is built without OpenSSL.

## Updating

Download `httplib.h`, `split.py` and `LICENSE` from the upstream tag, run
`python3 split.py -e cpp -o <dir>`, copy the two outputs and `LICENSE` here,
and update the version above. Re-check `CMakeLists.txt` if upstream adds tuning macros that are used
from the header.

[yhirose/cpp-httplib]: https://github.com/yhirose/cpp-httplib
