[Documentation](../README.md) › [Architecture](README.md) › Multimodal input

# Multimodal input

![Media travels from the request through extract_media, the template and prefill_media into the KV cache](../../assets/diagrams/svg/multimodal-path.svg)

## Loading and capability reporting

`onyx_engine::load` initializes an `mtmd_context` (`mtmd_init_from_file`) only
when `--mmproj` is passed; `mctx_` stays null otherwise, and
`has_mmproj()`/`supports_vision()`/`supports_audio()` all read straight
through to it (`mtmd_support_vision`/`mtmd_support_audio`), which is what
`GET /props`'s `modalities` and the per-request capability checks in
`server.cpp` report. Because a media chunk's KV positions can be neither
prefix-matched token-by-token nor partially discarded, loading a projector
force-disables `--cache-reuse` and `--context-shift` process-wide, with a
warning on stderr, mirroring `llama-server`.

## Marker rewrite

`server.cpp::extract_media()` walks a chat request's
`messages` before the template renders: each `image_url`/`input_audio`
content part is decoded (data: URI or bare base64 — `reject_media_url()`
throws on `http://`/`https://`/`file://` before any capability check runs)
into a `onyx_media_buffer`, and the part itself is replaced in place by a
`media_marker` part carrying `engine.media_marker()`
(`mtmd_get_marker(mctx_)`) — so the rendered prompt text contains one marker
token sequence per decoded buffer, in order, and `gp.prompt_text` +
`gp.media` (not `gp.prompt_tokens`) are what get passed to `generate()`.

## Media prefill at admission

`onyx_engine::prefill_media()` runs when a
newly admitted slot's request carries `media`: it never reuses KV
(`n_past` starts at 0 — media prompts are never prefix-matched, see the KV
reuse section above), calls `mtmd_helper_bitmap_init_from_buf` per buffer
and `mtmd_helper_eval_chunk_single` per resulting chunk, which runs the
vision/audio encoder and issues its own `llama_decode` calls directly
against `ctx_`. `n_past` is tracked strictly from `mtmd_helper_eval_chunk_single`'s
out-param rather than computed locally, because M-RoPE advances position
differently than the flat per-token counting the rest of the engine loop
uses. This is the one part of request handling that runs outside the shared
per-tick `llama_batch`: it **serializes the engine loop** for the duration of
one slot's media prefill (other slots' KV and the request queue are
otherwise unaffected — this is a momentary stall, not a global lock).
