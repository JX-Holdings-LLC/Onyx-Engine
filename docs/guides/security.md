[Documentation](../README.md) › [Guides](README.md) › Security

# Security

Onyx Engine is built to sit behind something you control, on the same machine or the same trusted network. This page says what it does to keep that safe, and, just as important, what it leaves to you.

## What the engine does

- **Binds loopback.** `--host` defaults to `127.0.0.1`. Nothing outside the machine can reach it until you ask.
- **Refuses to go public without a key.** A non-loopback `--host` (`0.0.0.0`, a LAN address) without `--api-key` exits 1 before anything is bound. `--allow-unauthenticated` overrides that, for when something in front of the engine (a firewall, a container network, an authenticating proxy) is doing the job; pass it deliberately.
- **Optional bearer auth.** With `--api-key KEY`, every route except `GET /health` needs `Authorization: Bearer KEY`. A wrong or missing key gets `401` in the standard error shape. The comparison takes the same time however much of the key matches, so the key cannot be found byte by byte by timing.
- **Guards loopback against DNS rebinding.** When bound to a loopback address, a request whose `Host` header names anything other than `localhost` (or a `*.localhost` name), `127.x.x.x` or `[::1]` gets `403`. A malicious page that rebinds its own name to `127.0.0.1` still sends that name in `Host`, so it is refused even on routes that need no key. Requests without a `Host` header (HTTP/1.0) are allowed, since browsers always send one.
- **Refuses browsers.** Any request with an `Origin` header gets `403`, on every route, `/health` included, before the key is even checked. `OPTIONS` gets `403`. A web page cannot call the engine directly.
- **Does no I/O on a request's behalf.** A request can name no URL and no file for the engine to open. Images and audio arrive as inline base64; `http://`, `https://` and `file://` are refused with `400`. There is no server-side request forgery surface to defend.
- **Treats control tokens in messages as text.** Special tokens are parsed only in template text, never in message content, so a user cannot smuggle in `<|im_end|>` and forge a turn boundary.
- **Validates its inputs.** Unsupported fields, bad token ids, oversized prompts and malformed grammars are `400`s, not crashes. Request bodies are capped at 100 MiB (`413` beyond).
- **Can shed load.** `--max-queue N` bounds the wait queue; beyond it, generation requests get `503` with `Retry-After`.
- **Cancels abandoned work.** A client that disconnects mid-stream has its request cancelled and its slot freed at the next tick.

## What it leaves to you

| Concern | Do this |
|---|---|
| **Transport encryption** | The engine speaks plain HTTP. Terminate TLS in a reverse proxy, or keep the traffic on loopback or an encrypted private network. |
| **Binding beyond loopback** | If you pass `--host 0.0.0.0` (or a LAN address), the engine requires `--api-key`. Firewall the port as well. The `Host` check applies only to loopback binds; on a public bind the key is the guard. |
| **Key hygiene** | `--api-key` appears in the process's argument list, which other local users may be able to read. Pass it from your supervisor's secret store, restrict who can list processes, and rotate it by restarting. |
| **Rate limits** | There are none per client. `--max-queue` bounds the total queue, but one client with a key can still fill it. Put per-client limits in the proxy. |
| **Who may use the key** | There is one key and no per-user identity or audit log. If you need either, authenticate in the proxy. |
| **Untrusted model files** | A GGUF is parsed by llama.cpp. Load models you trust, from sources you trust. |

## The converter runs a script

Pointing `-m` at a safetensors model makes the engine run `python3 scripts/convert-safetensors.py` as a child process, once, on a cache miss. The script is found through `$ONYX_ENGINE_CONVERT_SCRIPT` first, then beside the binary. Do not let an untrusted party set that variable or write next to the binary. For a locked-down host, convert models elsewhere and hand the engine a GGUF; then no child process is ever started.

## Supply chain

The llama.cpp source is fetched by exact 40-hex commit and verified through Git's object hashes; there is no registry package in the path of a normal build. [Vendoring](../development/vendoring.md) explains the pin, and the release pipeline [checksums every archive](../development/releasing.md). The release workflow pins the shared release infrastructure to one commit.

## Reporting

The repository has no security-policy file yet. Contact the maintainers privately rather than putting exploit details in a public issue.
