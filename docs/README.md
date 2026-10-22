<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="../assets/logo/svg/logo-on-dark.svg">
    <img alt="Onyx Engine" src="../assets/logo/svg/logo-on-light.svg" width="300">
  </picture>
</p>

# Onyx Engine documentation

This is the shop manual. The [root README](../README.md) tells you what the engine is and gets it running; these pages tell you how to run it well, what is going on under the hood, and how to take it apart and put it back together.

![Anatomy of the engine: callers, HTTP layer, request queue, engine loop and slots, llama.cpp and ggml](../assets/diagrams/svg/anatomy.svg)

## Find your page

| I want to... | Go to |
|---|---|
| Get it running in twenty minutes | [Getting started](getting-started.md) |
| Run it properly: contexts, slots, GPUs, templates | [Serving models](guides/serving-models.md) |
| Write a client | [API reference](reference/api/README.md) |
| Look up a flag | [Command line](reference/cli.md) |
| Know what it refuses to do | [Compatibility and limits](reference/compatibility.md) |
| Understand a `400` or a `501` | [Troubleshooting](guides/troubleshooting.md) |
| Connect JX Runtime | [JX Runtime integration](integration/jx-runtime.md) |
| Open the hood | [Architecture](architecture/README.md) |
| Build, test, vendor or release it | [Development](development/README.md) |
| See where it is going | [Roadmap](../ROADMAP.md) |

## The sections

### Start here

- [Getting started](getting-started.md): binary or source, first request, first stream, first test run.

### Guides

Task first, reasons after. [Index](guides/README.md).

- [Serving models](guides/serving-models.md): formats, aliases, context and slots, GPUs, templates
- [Structured output and tools](guides/structured-output-and-tools.md): schemas, grammars, tool calls
- [Multimodal input](guides/multimodal.md): images and audio, and what they cost
- [Embeddings and reranking](guides/embeddings-and-reranking.md)
- [Decision models](guides/decision-models.md): typed questions on `/v1/systemone`
- [Security](guides/security.md): what the engine does, and what it leaves to you
- [Troubleshooting](guides/troubleshooting.md): messages and their causes

### Reference

Exact and checked against the code. [Index](reference/README.md).

- [Command line](reference/cli.md)
- [API reference](reference/api/README.md): [service endpoints](reference/api/service-endpoints.md), [chat completions](reference/api/chat-completions.md), [text completions](reference/api/completions.md), [embeddings](reference/api/embeddings.md), [decision models](reference/api/systemone.md)
- [Compatibility and limits](reference/compatibility.md)

### Architecture

How it is built. [Index](architecture/README.md).

- [Concurrency and batching](architecture/concurrency.md)
- [KV cache: prefix reuse and context shift](architecture/kv-cache.md)
- [Multimodal input](architecture/multimodal.md)
- [Decision models](architecture/decision-models.md)
- [Safetensors resolution and conversion](architecture/safetensors-conversion.md)
- [Reasoning budget and logprobs](architecture/reasoning-and-logprobs.md)
- [Chat templates, grammar and sampling](architecture/chat-templates-and-sampling.md)
- [Build layering](architecture/build-layering.md)

### Integration

- [JX Runtime](integration/jx-runtime.md): the contract the `onyxengine` adapter is built against.

### Development

For maintainers. [Index](development/README.md).

- [Building](development/building.md), [Testing and CI](development/testing-and-ci.md), [Vendoring](development/vendoring.md), [Releasing](development/releasing.md), [Documentation](development/documentation.md)

### Planning and brand

- [Roadmap](../ROADMAP.md) and its two source reports in [`reports/`](../reports/README.md), with the evidence in [`research_notes/`](../research_notes/)
- [Changelog](../CHANGELOG.md)
- [Brand and logo](brand.md)

## Conventions on these pages

- Flags, fields and routes are written in `code`. A route is `METHOD /path`.
- "Slot" means one concurrent request lane with its own KV-cache sequence. "Tick" means one pass of the engine loop.
- Paths are relative to the repository root unless they start with a dot-dot.
- Pages describe what the engine does now. What changed, and when, is in the changelog.
