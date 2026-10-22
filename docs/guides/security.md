[Documentation](../README.md) › [Guides](README.md) › Security

# Security

Onyx Engine is built to sit behind something you control, on the same machine or the same trusted network. This page says what it does to keep that safe, and, just as important, what it leaves to you.

## What the engine does

- **Binds loopback.** `--host` defaults to `127.0.0.1`. Nothing outside the machine can reach it until you ask.
- **Optional bearer auth.** With `--api-key KEY`, every route except `GET /health` needs `Authorization: Bearer KEY`. A wrong or missing key gets `401` in the standard error shape.
- **Refuses browsers.** Any request with an `Origin` header gets `403`, on every route, `/health` included, before the key is even checked. `OPTIONS` gets `403`. A web page cannot call the engine directly.
- **Does no I/O on a request's behalf.** A request can name no URL and no file for the engine to open. Images and audio arrive as inline base64; `http://`, `https://` and `file://` are refused with `400`. There is no server-side request forgery surface to defend.
- **Treats control tokens in messages as text.** Special tokens are parsed only in template text, never in message content, so a user cannot smuggle in `<|im_end|>` and forge a turn boundary.
- **Validates its inputs.** Unsupported fields, bad token ids, oversized prompts and malformed grammars are `400`s, not crashes.
- **Cancels abandoned work.** A client that disconnects mid-stream has its request cancelled and its slot freed at the next tick.

## What it leaves to you

| Concern | Do this |
|---|---|
| **Transport encryption** | The engine speaks plain HTTP. Terminate TLS in a reverse proxy, or keep the traffic on loopback or an encrypted private network. |
| **Binding beyond loopback** | If you pass `--host 0.0.0.0` (or a LAN address), also pass `--api-key`, and firewall the port. The engine does not currently refuse a non-loopback bind without a key; the [roadmap](../../ROADMAP.md) lists doing so. |
| **Key hygiene** | `--api-key` appears in the process's argument list, which other local users may be able to read. Pass it from your supervisor's secret store, restrict who can list processes, and rotate it by restarting. The key is compared as a plain string today; a constant-time compare is on the roadmap. |
| **Rate limits and request caps** | There are none, and no explicit payload cap. A client with a key can queue work until the slots are saturated; the queue is first in, first out and unbounded. Put limits in the proxy. |
| **Who may use the key** | There is one key and no per-user identity or audit log. If you need either, authenticate in the proxy. |
| **Untrusted model files** | A GGUF is parsed by llama.cpp. Load models you trust, from sources you trust. |

## The converter runs a script

Pointing `-m` at a safetensors model makes the engine run `python3 scripts/convert-safetensors.py` as a child process, once, on a cache miss. The script is found through `$ONYX_ENGINE_CONVERT_SCRIPT` first, then beside the binary. Do not let an untrusted party set that variable or write next to the binary. For a locked-down host, convert models elsewhere and hand the engine a GGUF; then no child process is ever started.

## Supply chain

The llama.cpp source is fetched by exact 40-hex commit and verified through Git's object hashes; there is no registry package in the path of a normal build. [Vendoring](../development/vendoring.md) explains the pin, and the release pipeline [checksums every archive](../development/releasing.md). The release workflow pins the shared release infrastructure to one commit.

## Reporting

The repository has no security-policy file yet. Contact the maintainers privately rather than putting exploit details in a public issue.
