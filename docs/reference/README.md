[Documentation](../README.md) › Reference

# Reference

Look-up pages. They state what the code does today, and they are checked against `src/args.cpp` and `src/server.cpp` whenever either changes.

| Page | Read it when |
|---|---|
| [Command line](cli.md) | You need a flag, its default, or what a startup error means |
| [API reference](api/README.md) | You are writing a client: routes, request fields, response and stream shapes |
| [Stability](stability.md) | You are building on the engine and need to know what can change between versions |
| [Compatibility and limits](compatibility.md) | A request came back `400` or `501`, or you are checking whether something is in scope |

The API pages:

- [Service endpoints](api/service-endpoints.md): `/health`, `/props`, `/v1/models`, `/tokenize`, `/detokenize`, `/apply-template`
- [Chat completions](api/chat-completions.md): `/v1/chat/completions`
- [Text completions](api/completions.md): `/v1/completions`
- [Embeddings](api/embeddings.md): `/v1/embeddings`
- [Decision models](api/systemone.md): `/v1/systemone`
