[Documentation](../README.md) › [Development](README.md) › Documentation

# Documentation

## How the docs are laid out

The [documentation home](../README.md) is organized by what the reader is trying to do, not by when a page was written.

| Folder | For | Voice |
|---|---|---|
| `docs/getting-started.md` | the first twenty minutes | plain, step by step |
| `docs/guides/` | doing a particular job with the engine | task first, then the reasons |
| `docs/reference/` | looking something up | exact; no story |
| `docs/architecture/` | understanding or changing the engine | mechanism, with the source files named |
| `docs/integration/` | connecting a caller | the contract, from the engine's side |
| `docs/development/` | building, testing, vendoring, releasing | maintainer to maintainer |
| `ROADMAP.md`, `reports/`, `research_notes/` | planning | evidence and decisions |

The root [README](../../README.md) is the front door and carries the shop-talk. Everything under `docs/` stays plain and exact.

## Rules we keep

- **The code wins.** Reference pages are checked against `src/args.cpp` and `src/server.cpp`. When they disagree, fix the page, or the code, in the same change.
- **One home per fact.** Defaults live in [the CLI reference](../reference/cli.md), field behavior in [the API pages](../reference/api/README.md), mechanisms in [architecture](../architecture/README.md). Other pages link to them.
- **Say what is not supported.** A refusal belongs in [compatibility and limits](../reference/compatibility.md) as well as on the endpoint page.
- **Do not date the prose.** Write "the engine does X", not "as of v2 the engine now does X". History belongs in the [changelog](../../CHANGELOG.md).
- **Relative links only,** and every link must resolve. The check below runs in a few seconds.

## Checking links

```bash
python3 - <<'PY'
import pathlib, re, sys
root = pathlib.Path(".")
bad = 0
for md in root.rglob("*.md"):
    if "node_modules" in md.parts or "third_party" in md.parts:
        continue
    for target in re.findall(r"\]\(([^)#\s]+)(?:#[^)]*)?\)", md.read_text(encoding="utf-8")):
        if "://" in target or target.startswith("mailto:"):
            continue
        if not (md.parent / target).resolve().exists():
            print(f"{md}: missing {target}")
            bad += 1
sys.exit(1 if bad else 0)
PY
```

## Diagrams

The diagrams in `assets/diagrams/svg/` are layered [Vixl](https://github.com/jxburros/Vixl) documents exported to SVG, drawn in the logo's palette (see [brand](../brand.md)). They are built by [`assets/diagrams/build.py`](../../assets/diagrams/build.py) from the helpers in [`lib.py`](../../assets/diagrams/lib.py). With `vixl` on `PATH` (it downloads Space Grotesk on first run):

```bash
python assets/diagrams/build.py                      # rebuild every diagram in place
python assets/diagrams/build.py request-lifecycle    # rebuild one
python assets/diagrams/build.py /tmp/out             # or write somewhere else
ONYX_PREVIEW=/tmp/png python assets/diagrams/build.py   # also write PNG previews
```

The output is deterministic, so an unchanged script produces no diff. To add a diagram, write a function decorated with `@diagram` in `build.py`, build it, look at the PNG preview, and reference `assets/diagrams/svg/<name>.svg` from the page that needs it. Reuse the helpers (`box`, `group`, `arrow`, `hexbox`, `badge`, `chip`, `cell`) so a new diagram looks like the rest: graphite parts with steel edges, cyan for the live path, and amber only for what stalls, refuses or stops.

| Diagram | Used on |
|---|---|
| `anatomy` | README, architecture |
| `request-lifecycle`, `batching-tick`, `context-slots` | architecture: concurrency |
| `boot-sequence`, `source-map` | architecture |
| `kv-reuse-and-shift` | architecture: KV cache |
| `multimodal-path` | architecture: multimodal |
| `decision-models` | architecture and guide: decision models |
| `safetensors-conversion` | architecture: safetensors |
| `reasoning-budget` | architecture: reasoning |
| `build-layers` | architecture: build layering |
| `route-modes` | API reference |
| `jx-runtime` | integration |
| `vendor-pipeline` | development: vendoring |
| `release-pipeline` | development: releasing |
| `roadmap-map` | roadmap |
