[Documentation](../docs/README.md) › Reports

# Reports

Four planning documents; the first two are reports written on 2026-10-21 against the llama.cpp pin of the time (b10711), and the evidence behind them. [`ROADMAP.md`](../ROADMAP.md) summarises both and decides nothing the reports do not; the pin has since moved to b11820, which the roadmap accounts for.

| Report | Question it answers | Evidence |
|---|---|---|
| [Onyx Engine improvement opportunities](Onyx%20Engine%20improvement%20opportunities.md) | What should be ported from llama-server, what hardening is missing, which API surfaces would unlock today's agent clients, and what to do about the converter | [`research_notes/Onyx Engine improvement opportunities/`](../research_notes/Onyx%20Engine%20improvement%20opportunities/): API standards, engineering and security, models and conversion, serving techniques, upstream llama.cpp |
| [Replacing llama cpp common code](Replacing%20llama%20cpp%20common%20code.md) | For each piece of llama.cpp's `common/` library: replace it, copy it, or keep it, and in what stages | [`research_notes/Replacing llama cpp common code/`](../research_notes/Replacing%20llama%20cpp%20common%20code/): chat templates and tools, KV-cache state, sampling and grammar, speculative decoding |
| [Making Onyx Engine an independent tool](independent-tool-plan.md) | What to change so people outside the JX ecosystem can build, run and depend on the engine, in what order | The repo as of 2026-10-22; no separate notes. A plan, not an analysis of upstream |
| [Cross-repo action items (2026-10-31)](2026-10-31-cross-repo-action-items.md) | What Onyx Engine should adopt from Media Engine so JX Runtime can check it, which decisions that needs, and which inconsistencies a cross-repo review found | The cross-repo review of Media Runtime, Media Engine, JX Runtime and Onyx Engine, committed in Media-Runtime as `docs/reviews/2026-10-31-cross-repo-review.md`; read, not run |

The reports are dated analysis, not documentation: they are not edited when the code moves. The documents that describe the engine as it is today live under [`docs/`](../docs/README.md).
