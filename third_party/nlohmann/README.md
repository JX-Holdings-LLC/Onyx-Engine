# nlohmann/json (vendored)

`nlohmann/json.hpp` and `nlohmann/json_fwd.hpp` from [nlohmann/json], version **3.12.0**, with
the deep-nesting backport in `json-deep-nesting.patch` already applied. They are byte-for-byte
the copy the pinned llama.cpp (b11820) vendors in `vendor/nlohmann/`, so `llama-common`, which
uses the same header inside `common_json`, and Onyx agree on one version. `LICENSE.MIT` is that
project's licence.

SHA-256:

```
08f19bbcc66ff3082f5caf58867b1232987dba98d82e2b29d870722fce42776b  nlohmann/json.hpp
fb6aa70cbece087f37ab4685c182b287c53be54f785f981b9db9d30d2d028b37  nlohmann/json_fwd.hpp
```

## Why this is here

Stage 1 of the [`common/` plan](../../reports/Replacing%20llama%20cpp%20common%20code.md): Onyx's
own code (`src/server.cpp`, `src/decision.cpp`, the chat seam) uses `nlohmann::ordered_json`
through `src/onyx_json.h` instead of llama.cpp's `common_json` wrapper, so a pin bump that
reshapes `common/json.h` does not touch the HTTP layer. Ordered objects keep keys in insertion
order, which is what `common_json` did, so response bodies are unchanged. Values cross into
llama.cpp's chat layer and schema converter as text, in `src/onyx_chat.cpp` and
`onyx_json_to_common()`.

The patch backports upstream's fixes for the stack overflow when copying, dumping or comparing
a deeply nested value (nlohmann/json#5387). Onyx also refuses request bodies nested deeper than
64 levels before parsing them. Drop the patch, and take a plain upstream release, once
nlohmann/json releases a version newer than 3.12.0.

[nlohmann/json]: https://github.com/nlohmann/json
