[Documentation](../README.md) › Development

# Development

For people building, testing, vendoring or releasing the engine.

| Page | Covers |
|---|---|
| [Building](building.md) | Prerequisites, fetching the pinned llama.cpp, CPU and GPU builds, the install target |
| [Testing and CI](testing-and-ci.md) | The offline smoke and conversion tests, the CI matrix, the nightly QA profile |
| [Vendoring](vendoring.md) | Vendor specs, building and publishing the source packages, keeping the ggml pin in step |
| [Releasing](releasing.md) | The manual Release workflow, what gets published, and what the first attempt taught us |
| [Documentation](documentation.md) | How these docs are organized, and how to rebuild the diagrams |

## The shape of a change

1. Build and run the smoke test on your own machine (`npm ci`, `npm run build`, `npm test`).
2. Keep the parser and `--help` moving together: a flag is in both or in neither.
3. Update the docs in the same change: [`docs/reference/cli.md`](../reference/cli.md) for a flag, the [API pages](../reference/api/README.md) for a field or route, the [architecture notes](../architecture/README.md) for a mechanism. [Documentation](documentation.md) lists the rest.
4. Advance the version. The `every-main-change` policy applies to docs-only changes too; see [releasing](releasing.md).

## Where things are

| Path | What lives there |
|---|---|
| `src/` | The six source files described in [architecture](../architecture/README.md#source-layout) |
| `scripts/` | Build, vendor, test-model generators, the safetensors converter, QA checks |
| `packaging/` | Vendor specs and the source-package builder |
| `third_party/cpp-httplib/` | A verbatim copy of cpp-httplib v0.54.1 (MIT) with its provenance notes |
| `.github/workflows/` | `ci.yml`, `release.yml`, `release-validation.yml`, `publish-vendor.yml` |
| `.release/` | The unified-release configuration, version glue and per-version notes |
| `.ai/` | The QA runner's profile and instructions |
| `assets/logo/`, `assets/diagrams/` | The brand pack and the documentation diagrams, both built with Vixl |
| `reports/`, `research_notes/` | Planning reports and the evidence behind them |
