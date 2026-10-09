"""Marketing kits for JX Runtime, Onyx Engine, and the two together, all made with Vixl.

The same script lives in both repositories (JX-Runtime/marketing/ and Onyx-Engine/marketing/).
Run it from anywhere with Vixl 0.23.0 or newer installed (`pip install "vixl-engine[pdf]"`):

    python marketing/build.py                    # this repo's own kit plus the joint kit
    python marketing/build.py --kit together     # one kit
    python marketing/build.py --kit jx og deck   # some pieces of one kit

Each piece is written to marketing/output/<kit>/<folder>/ as an editable .vixl master plus its
exports and a .check.json with the `vixl check` findings it shipped with. The logos are drawn from
vector shapes in this file (`jx_mark`, `onyx_mark`), so changing them here changes every piece.
Fonts (Archivo, Space Grotesk, JetBrains Mono) download from Google Fonts on the first run and
are embedded in each master. The MP4 teasers need ffmpeg.
"""

import argparse
import json
import math
import os
from pathlib import Path

os.environ.setdefault("VIXL_NO_UPDATE", "1")

from vixl import Project  # noqa: E402
from vixl.render import resolve_layout  # noqa: E402
from vixl.typefaces import install_font  # noqa: E402

HERE = Path(__file__).resolve().parent
OUT = HERE / "output"
REPO_KIT = "jx" if (HERE.parent / "src" / "backends").is_dir() else "onyx"

MONO = "jetbrains-mono-500"
CODE_KEY, CODE_STR, CODE_NUM, CODE_DIM = "#9CC4FF", "#A7E8BD", "#FFC98A", "#7D7987"

# Brands -----------------------------------------------------------------------------------------
# JX Runtime takes its palette and type from its own UI (src/ui/index.html): carbon blacks,
# chrome text, the "hot" crimson accent, Archivo. Onyx Engine is the black stone with a teal edge.
JX = {
    "key": "jx", "name": "JX Runtime", "bg": "#0A090C", "deep": "#050506", "panel": "#17171B",
    "panel2": "#121016", "line": "#2E2B35", "text": "#F1EEE9", "muted": "#B9B5C0", "accent": "#FF2D55",
    "accent2": "#FF8FA6", "accent_deep": "#8C0C2A", "paper": "#F4F2EE", "ink": "#121016", "subtle": "#55525C",
    "head": "archivo-900", "body": "archivo-400", "semi": "archivo-600",
    "fonts": [("Archivo", 900), ("Archivo", 400), ("Archivo", 600)],
    "repo": "github.com/jxburros/JX-Runtime",
}
ONYX = {
    "key": "onyx", "name": "Onyx Engine", "bg": "#07090B", "deep": "#030405", "panel": "#11171B",
    "panel2": "#0C1114", "line": "#22313A", "text": "#E6F2F1", "muted": "#9AAEB2", "accent": "#2DD4BF",
    "accent2": "#99F6E4", "accent_deep": "#0F766E", "paper": "#EEF4F3", "ink": "#0C1114", "subtle": "#4E5F64",
    "head": "space-grotesk-700", "body": "space-grotesk-400", "semi": "space-grotesk-500",
    "fonts": [("Space Grotesk", 700), ("Space Grotesk", 400), ("Space Grotesk", 500)],
    "repo": "github.com/JX-Holdings-LLC/Onyx-Engine",
}
# The joint kit keeps JX's carbon and type, and carries both accents: crimson for the runtime,
# teal for the engine, and a crimson-to-teal gradient wherever the two meet.
TOGETHER = {**JX, "key": "together", "name": "JX Runtime + Onyx Engine", "bg": "#09090B", "panel": "#151619",
            "line": "#2C2D33", "teal": ONYX["accent"], "teal2": ONYX["accent2"],
            "fonts": JX["fonts"] + ONYX["fonts"]}


# Plumbing ---------------------------------------------------------------------------------------
class Kit:
    """A brand plus the output folder for its pieces."""

    def __init__(self, brand):
        self.b = brand
        self.out = OUT / {"jx": "jx-runtime", "onyx": "onyx-engine", "together": "together"}[brand["key"]]

    def new(self, width, height, background=None):
        b = self.b
        p = Project(width, height, background or b["bg"])
        for family, weight in b["fonts"]:
            install_font(p, family, weight)
        install_font(p, "JetBrains Mono", 500)
        return p

    def save(self, p, folder, stem, exports, check=True, **check_options):
        folder_path = self.out / folder
        folder_path.mkdir(parents=True, exist_ok=True)
        p.save(str(folder_path / f"{stem}.vixl"), overwrite=True)
        if check:
            result = p.check(**check_options)
            issues = [{k: i.get(k) for k in ("check", "severity", "action", "layer", "message")}
                      for i in result.get("issues", [])]
            fixes = [i for i in issues if i["action"] == "fix"]
            print(f"  check {stem}: {len(issues)} issues, {len(fixes)} to fix")
            for issue in fixes:
                print("    FIX", issue["check"], (issue["message"] or "")[:160])
            (folder_path / f"{stem}.check.json").write_text(json.dumps({"issues": issues}, indent=2) + "\n")
        for ext, options in exports:
            path = folder_path / f"{stem}.{ext}"
            p.export(str(path), overwrite=True, **options)
            print("  wrote", path.relative_to(HERE))

    def sheet(self, p, folder, name, **options):
        from vixl.deck import contact_sheet

        path = self.out / folder / name
        contact_sheet(p, **options).convert("RGB").save(path)
        print("  wrote", path.relative_to(HERE))


def all_layers(p):
    stack, found = list(p.state["layers"]), []
    while stack:
        layer = stack.pop()
        found.append(layer)
        stack.extend(layer.get("children", []))
    return found


def bounds(p, name):
    """Ink bounds of a layer: (x, y, width, height) in canvas pixels.

    resolve_layout reports a group's children relative to the group, so add up the ancestors."""
    parent, by_name = {}, {}
    for layer in all_layers(p):
        by_name.setdefault(layer["name"], layer["id"])
        if layer.get("parent"):
            parent[layer["id"]] = layer["parent"]
    layout = resolve_layout(p)
    ident = by_name.get(name)
    if ident is None or ident not in layout:
        raise KeyError(name)
    x, y, w, h = layout[ident]
    up = parent.get(ident)
    while up is not None:
        x, y = x + layout[up][0], y + layout[up][1]
        up = parent.get(up)
    return x, y, w, h


def bottom(p, name):
    _, y, _, h = bounds(p, name)
    return y + h


def right(p, name):
    x, _, w, _ = bounds(p, name)
    return x + w


def move_ink(p, name, x=None, y=None, cx=None, cy=None, rx=None):
    """Move a layer so its ink box sits at x/y, is centred on cx/cy, or ends at rx."""
    bx, by, bw, bh = bounds(p, name)
    dx = dy = 0
    if x is not None:
        dx = x - bx
    if cx is not None:
        dx = cx - (bx + bw / 2)
    if rx is not None:
        dx = rx - (bx + bw)
    if y is not None:
        dy = y - by
    if cy is not None:
        dy = cy - (by + bh / 2)
    if dx or dy:
        p.apply({"type": "translate", "target": name, "dx": round(dx), "dy": round(dy)}, detail="brief")


def text(name, value, x, y, size, color, font, **extra):
    return {"type": "text", "name": name, "text": value, "font": font, "size": size, "color": color,
            "x": x, "y": y, **extra}


def para(name, value, x, y, size, width, color, font, line_height=1.32, align="left"):
    return {"type": "rich-text", "name": name, "spans": [{"text": value}], "font": font, "size": size,
            "color": color, "line_height": line_height, "x": x, "y": y, "width": width, "align": align}


def headline(b, name, value, x, y, size, accent=None, color=None, width=None, align="left", line_height=0.95,
             accent_color=None):
    """Display type as rich text so one phrase can take the accent colour."""
    spans = [{"text": value}]
    if accent and accent in value:
        before, after = value.split(accent, 1)
        spans = [s for s in ({"text": before}, {"text": accent, "color": accent_color or b["accent"]},
                             {"text": after}) if s["text"]]
    op = {"type": "rich-text", "name": name, "spans": spans, "font": b["head"], "size": size,
          "color": color or b["text"], "line_height": line_height, "align": align, "x": x, "y": y}
    if width:
        op["width"] = width
    return op


def rect(name, x, y, w, h, fill, radius=0, **extra):
    shape = "rounded-rectangle" if radius else "rectangle"
    op = {"type": "shape", "shape": shape, "name": name, "x": round(x), "y": round(y), "width": round(w),
          "height": round(h), "fill": fill, **extra}
    if radius:
        op["radius"] = radius
    return op


def glow(name, x, y, size, color, strength="55"):
    return {"type": "gradient", "name": name, "direction": "radial", "width": size, "height": size, "x": x, "y": y,
            "stops": [{"offset": 0, "color": f"{color}{strength}"}, {"offset": 0.6, "color": f"{color}10"},
                      {"offset": 1, "color": f"{color}00"}]}


def decoration(*names):
    return [{"type": "layer-intent", "target": n, "role": "decoration"} for n in names]


def dot_grid(name, x, y, width, height, step, color, dot=3):
    return [
        {"type": "shape", "shape": "ellipse", "name": name, "x": x, "y": y, "width": dot, "height": dot,
         "fill": color},
        {"type": "repeat", "target": name, "count": max(1, width // step), "dx": step, "dy": 0},
        {"type": "group", "name": f"{name}-row", "targets": [name]},
        {"type": "repeat", "target": f"{name}-row", "count": max(1, height // step), "dx": 0, "dy": step},
        {"type": "layer-intent", "target": f"{name}-row", "role": "decoration"},
    ]


def scanlines(name, x, y, width, height, step, color):
    """Faint horizontal rules: the runtime's 'rack' texture."""
    return [rect(name, x, y, width, 1, color),
            {"type": "repeat", "target": name, "count": max(1, height // step), "dx": 0, "dy": step},
            {"type": "group", "name": f"{name}-set", "targets": [name]},
            {"type": "layer-intent", "target": f"{name}-set", "role": "decoration"}]


def link_line(name, x, y, w, h=4, a=JX["accent"], b=ONYX["accent"]):
    """The crimson-to-teal bar used wherever the runtime meets the engine."""
    return {"type": "gradient", "name": name, "direction": "horizontal", "width": round(w), "height": h,
            "x": round(x), "y": round(y), "stops": [{"offset": 0, "color": a}, {"offset": 1, "color": b}]}


def chip(p, b, name, label, x, y, size=22, fg=None, bg=None, stroke=None, pad=(18, 10), font=None, dot=None):
    """Pill label: text first (to measure its ink box), then a capsule behind it."""
    fg, bg, font = fg or b["text"], bg or b["panel"], font or b["semi"]
    lead = round(size * 0.9) if dot else 0
    p.apply(text(f"{name}-label", label, x + pad[0] + lead, y, size, fg, font), detail="brief")
    _, ly, lw, lh = bounds(p, f"{name}-label")
    h = round(size * 0.72 + 2 * pad[1] + size * 0.3)
    w = round(lw + 2 * pad[0] + lead)
    ops = [rect(f"{name}-bg", x, y, w, h, bg, radius=h / 2, **({"stroke": stroke, "stroke_width": 2} if stroke else {}))]
    names = [f"{name}-bg", f"{name}-label"]
    if dot:
        d = round(size * 0.42)
        ops.append({"type": "shape", "shape": "ellipse", "name": f"{name}-dot", "x": x + pad[0], "y": round(y + (h - d) / 2),
                    "width": d, "height": d, "fill": dot})
        names.append(f"{name}-dot")
    p.apply(ops + [{"type": "lower", "target": f"{name}-bg"}], detail="brief")
    move_ink(p, f"{name}-label", cy=y + h / 2)
    p.apply({"type": "group", "name": name, "targets": names}, detail="brief")
    return x, y, w, h


def chips_row(p, b, prefix, labels, x, y, gap=12, **kw):
    cx, boxes = x, []
    for i, label in enumerate(labels):
        box = chip(p, b, f"{prefix}-{i}", label, cx, y, **kw)
        boxes.append(box)
        cx += box[2] + gap
    return boxes


def code_card(b, name, x, y, width, lines, size=22, title="terminal", chrome=True):
    """A dark terminal window; each line is (indent, [(text, colour), ...])."""
    top = round(size * 2.6) if chrome else round(size * 1.2)
    line_h = size * 1.5
    h = round(top + size * 1.4 + line_h * len(lines))
    ops = [rect(f"{name}-bg", x, y, width, h, b["deep"], radius=18, stroke=b["line"], stroke_width=2)]
    if chrome:
        for i, c in enumerate(["#FF5F57", "#FEBC2E", "#28C840"]):
            ops.append({"type": "shape", "shape": "ellipse", "name": f"{name}-dot{i}", "x": x + 24 + i * 22,
                        "y": y + round(top / 2) - 6, "width": 12, "height": 12, "fill": c})
        ops.append(text(f"{name}-title", title, x + 100, y + round(top / 2) - 14, max(18, round(size * 0.75)),
                        b["muted"], MONO))
    spans, paragraphs = [], []
    for i, (indent, runs) in enumerate(lines):
        spans.extend({"text": t, "color": c} for t, c in runs)
        if i < len(lines) - 1:
            spans.append({"text": "\n"})
        paragraphs.append({"indent": round(indent * size * 0.6)})
    ops.append({"type": "rich-text", "name": f"{name}-code", "spans": spans, "paragraphs": paragraphs,
                "font": MONO, "size": size, "color": b["text"], "line_height": 1.3, "x": x + 28,
                "y": y + top + 4, "width": width - 56})
    ops.append({"type": "group", "name": name, "targets": [o["name"] for o in ops]})
    return ops


def fit_code(p, name, pad=28):
    _, by, _, _ = bounds(p, f"{name}-bg")
    p.apply({"type": "resize", "target": f"{name}-bg", "height": round(bottom(p, f"{name}-code") + pad - by)},
            detail="brief")


# Marks ------------------------------------------------------------------------------------------
def jx_mark(prefix, x, y, size, light=False):
    """JX Runtime mark: a carbon module with the JX monogram and a lit crimson status LED."""
    tile = "#FFFFFF" if light else JX["panel"]
    edge = "#D9D5DE" if light else "#3A3842"
    ink = JX["ink"] if light else JX["text"]
    s = size
    led = max(4, round(s * 0.1))
    ops = [
        rect(f"{prefix}-tile", x, y, s, s, tile, radius=round(s * 0.22), stroke=edge, stroke_width=max(2, round(s * 0.02)),
             stroke_align="inside"),
        rect(f"{prefix}-rail", x + s * 0.2, y + s * 0.78, s * 0.6, max(2, s * 0.035), JX["accent"], radius=max(1, s * 0.02)),
        {"type": "shape", "shape": "ellipse", "name": f"{prefix}-halo", "x": round(x + s * 0.76 - led * 0.6),
         "y": round(y + s * 0.15 - led * 0.6), "width": round(led * 2.2), "height": round(led * 2.2),
         "fill": f"{JX['accent']}38"},
        {"type": "shape", "shape": "ellipse", "name": f"{prefix}-led", "x": round(x + s * 0.76), "y": round(y + s * 0.15),
         "width": led, "height": led, "fill": JX["accent"]},
        text(f"{prefix}-jx", "JX", x, y, round(s * 0.44), ink, JX["head"]),
    ]
    return ops


def place_jx_mark(p, prefix, x, y, size, light=False):
    p.apply(jx_mark(prefix, x, y, size, light), detail="brief")
    move_ink(p, f"{prefix}-jx", cx=x + size * 0.48, cy=y + size * 0.47)
    p.apply({"type": "group", "name": prefix, "targets": [f"{prefix}-{n}" for n in
                                                       ("tile", "rail", "halo", "led", "jx")]}, detail="brief")


def hex_points(cx, cy, r, squash=1.0):
    return [(cx + r * math.cos(math.radians(-90 + 60 * i)), cy + r * squash * math.sin(math.radians(-90 + 60 * i)))
            for i in range(6)]


def path_d(points):
    return "M" + " L".join(f"{px:.1f} {py:.1f}" for px, py in points) + " Z"


def onyx_mark(prefix, x, y, size, light=False):
    """Onyx Engine mark: a faceted black hexagonal stone, its facets edged in teal light."""
    s = size
    c = s / 2
    outer = hex_points(c, c, s / 2)
    inner = hex_points(c, c * 0.92, s * 0.25, 0.9)
    shades = ["#1D4A47", "#16262B", "#0E1518", "#070B0D", "#0B1114", "#1A2E33"]  # lit from the top right
    if light:
        shades = ["#25605B", "#22343A", "#162125", "#0B1215", "#121B1F", "#27403F"]
    ops = []
    for i in range(6):
        j = (i + 1) % 6
        ops.append({"type": "shape", "shape": "path", "name": f"{prefix}-f{i}", "x": x, "y": y,
                    "path": path_d([outer[i], outer[j], inner[j], inner[i]]), "fill": shades[i],
                    "stroke": ONYX["accent"] if i in (0, 5) else "#2DD4BF55", "stroke_width": max(1, round(s * 0.012)),
                    "line_join": "round"})
    ops.append({"type": "shape", "shape": "path", "name": f"{prefix}-table", "x": x, "y": y,
                "path": path_d(inner), "fill": "#0A1013", "stroke": ONYX["accent2"],
                "stroke_width": max(1, round(s * 0.016)), "line_join": "round"})
    ops.append({"type": "group", "name": prefix, "targets": [o["name"] for o in ops]})
    return ops


def lockup(p, b, prefix, x, y, height, light=False, show_sub=False):
    """Mark plus wordmark. Returns the lockup's right edge."""
    if b["key"] == "onyx":
        p.apply(onyx_mark(f"{prefix}-mark", x, y, height, light), detail="brief")
        words, rest = "Onyx", " Engine"
    else:
        place_jx_mark(p, f"{prefix}-mark", x, y, height, light)
        words, rest = "JX", " Runtime"
    ink = b["ink"] if light else b["text"]
    tail = b["subtle"] if light else b["muted"]
    p.apply({"type": "rich-text", "name": f"{prefix}-word", "font": b["head"], "size": round(height * 0.5),
             "color": ink, "x": x + round(height * 1.28), "y": y,
             "spans": [{"text": words}, {"text": rest, "color": tail}]}, detail="brief")
    move_ink(p, f"{prefix}-word", x=x + round(height * 1.25), cy=y + height / 2)
    p.apply({"type": "group", "name": prefix, "targets": [f"{prefix}-mark", f"{prefix}-word"]}, detail="brief")
    return right(p, prefix)


def joint_lockup(p, prefix, x, y, height, light=False):
    """Both lockups joined by a plus sign."""
    r = lockup(p, JX, f"{prefix}-jx", x, y, height, light)
    gap = round(height * 0.45)
    p.apply(text(f"{prefix}-plus", "+", r + gap, y, round(height * 0.62), JX["muted"] if not light else JX["subtle"],
                 JX["semi"]), detail="brief")
    move_ink(p, f"{prefix}-plus", x=r + gap, cy=y + height / 2)
    r2 = lockup(p, ONYX, f"{prefix}-onyx", right(p, f"{prefix}-plus") + gap, y, height, light)
    p.apply({"type": "group", "name": prefix, "targets": [f"{prefix}-jx", f"{prefix}-plus", f"{prefix}-onyx"]},
            detail="brief")
    return r2


# Copy -------------------------------------------------------------------------------------------
# Every claim below is taken from the two READMEs and docs as of JX Runtime 1.4.4 and
# Onyx Engine 0.4.0. Keep it that way: change the copy when the product changes.
COPY = {
    "jx": {
        "kicker": "JX RUNTIME  /  LOCAL-FIRST MODEL RUNTIME",
        "hero": "Local models,\nsafe to share.", "accent": "safe to share.",
        "hero_long": "The local model runtime\nyou can expose to someone\nother than yourself.",
        "hero_long_accent": "other than yourself.",
        "sub": "Serve GGUF and safetensors models over an OpenAI-compatible API, with scoped keys, TLS, "
               "device pairing and an audit log on by default.",
        "chips": ["OpenAI-compatible", "Keys on by default", "TLS + pinning", "Local-first"],
        "motto": "The runtime executes. The application decides.",
        "problem_head": "Local AI is easy.\nSharing it safely\nisn't.", "problem_accent": "isn't.",
        "problem_deck": "Local AI is easy.\nSharing it safely isn't.", "problem_deck_accent": "Sharing it safely isn't.",
        "problems": [("An open port", "A bare local server reached over plain HTTP answers whoever finds it."),
                     ("Bolt-on security", "Keys, TLS and rate limits are left for the operator to find and assemble."),
                     ("Silent settings", "Offload and context get chosen for you, with no word on why.")],
        "features": [("Scoped access keys", "Per-key permissions, model lists, expiry, IP limits and rate limits. On by default."),
                     ("Built-in TLS", "HTTPS with a generated certificate and a fingerprint you can pin."),
                     ("Device pairing", "A single-use JX-XXXX-XXXX code mints a scoped key for one device."),
                     ("Postures, not ports", "local, lan, private-network or tunnel: the mode says who can reach you."),
                     ("Audit log", "Key lifecycle, denials, pairing outcomes and rate-limit trips, on record."),
                     ("Auto, explained", "GPU offload, threads, batch and context chosen for you, each with a reason.")],
        "stats": [("1", "required dependency"), ("4", "named network postures"), ("4", "engine backends"),
                  ("0", "prompts in logs or telemetry")],
        "privacy": "Prompts and outputs never leave the machine, by default.",
        "quick": [(0, [("$ ", None), ("curl -fsSL …/scripts/install.sh | sh", "t")]),
                  (0, [("$ ", None), ("jx-runtime setup", "t")]),
                  (0, [("# engine, a model that fits, a key, start at login", "d")])],
        "call": [(0, [("$ ", None), ("export BASE=https://127.0.0.1:8712/v1", "t")]),
                 (0, [("$ ", None), ("curl $BASE/chat/completions \\", "t")]),
                 (1, [("-H ", "t"), ("\"Authorization: Bearer jxr_…\"", "s"), (" \\", "t")]),
                 (1, [("-d ", "t"), ("'{\"model\":\"qwen\",\"messages\":[…]}'", "s")])],
        "feature_titles": [("Built in,\nnot bolted on.", "not bolted on."), ("Honest about\nevery choice.", "every choice.")],
        "words": ["Keys.", "TLS.", "Pairing.", "Audit.", "Explained."],
        "status": "Alpha. Release binaries are not yet code-signed.",
        "end": "Local models, safe to share.",
    },
    "onyx": {
        "kicker": "ONYX ENGINE  /  BUILT ON LLAMA.CPP",
        "hero": "One model.\nOne process.\nOpenAI-ready.", "accent": "OpenAI-ready.",
        "hero_long": "A single-model,\nOpenAI-compatible server\nbuilt on llama.cpp.",
        "hero_long_accent": "built on llama.cpp.",
        "sub": "Chat, completions and embeddings from one GGUF or safetensors model, with streaming, "
               "continuous batching, tool calls and structured output.",
        "chips": ["llama.cpp inside", "OpenAI-compatible", "MIT licensed", "4 prebuilt platforms"],
        "motto": "One model per process. Nothing fetched on a request's behalf.",
        "problem_head": "Serving one model\nshould be one\ncommand.", "problem_accent": "one\ncommand.",
        "problem_deck": "Serving one model\nshould be one command.", "problem_deck_accent": "one command.",
        "problems": [("Glue code", "Wrapping an engine in an OpenAI-shaped API is work every app repeats."),
                     ("Queued requests", "One request at a time wastes the hardware you already have."),
                     ("Format friction", "A Hugging Face model directory isn't a GGUF file, and conversion drags in torch.")],
        "features": [("Continuous batching", "--parallel N slots share one batch per tick, so requests generate together."),
                     ("Structured output", "GBNF grammars, json_schema and response_format."),
                     ("Tool calls", "Jinja chat templates, with parsed and streamed tool-call deltas."),
                     ("Context shift", "A slot that fills its context drops the oldest tokens and keeps going."),
                     ("Multimodal, no fetching", "Image and audio as data: URIs only. The engine performs no I/O for a request."),
                     ("Safetensors to GGUF", "A numpy-only converter turns a Hugging Face Llama directory into GGUF on first load.")],
        "stats": [("1", "model per process"), ("9", "HTTP endpoints"), ("4", "prebuilt platforms"),
                  ("6", "build targets: CPU to ROCm")],
        "privacy": "Remote media URLs are refused with 400, by design.",
        "quick": [(0, [("$ ", None), ("npm ci && npm run build", "t")]),
                  (0, [("$ ", None), ("./build/onyx-engine -m model.gguf --port 8080", "t")]),
                  (0, [("# add -np 4 for four concurrent slots", "d")])],
        "call": [(0, [("$ ", None), ("curl localhost:8080/v1/chat/completions \\", "t")]),
                 (1, [("-H ", "t"), ("\"Content-Type: application/json\"", "s"), (" \\", "t")]),
                 (1, [("-d ", "t"), ("'{\"messages\": [{\"role\": \"user\",", "s")]),
                 (4, [("\"content\": \"Hello!\"}]}'", "s")])],
        "feature_titles": [("Fast where\nit counts.", "it counts."), ("Careful by\ndesign.", "by design.")],
        "words": ["Stream.", "Batch.", "Call tools.", "Constrain.", "Embed."],
        "status": "v0.4.0. Prebuilt for linux-x64, darwin-arm64, darwin-x64 and win32-x64.",
        "end": "One model. One process.",
    },
}

ENDPOINTS = [("GET", "/health"), ("GET", "/props"), ("GET", "/v1/models"), ("POST", "/tokenize"),
             ("POST", "/detokenize"), ("POST", "/apply-template"), ("POST", "/v1/chat/completions"),
             ("POST", "/v1/completions"), ("POST", "/v1/embeddings")]

JOINT = {
    "kicker": "JX RUNTIME  +  ONYX ENGINE",
    "hero": "Onyx runs the model.\nJX guards the door.", "accent": "JX guards the door.",
    "hero_tall": "Onyx runs\nthe model.\nJX guards\nthe door.", "tall_accent": "JX guards\nthe door.",
    "hero_short": "The engine and\nthe runtime,\nbuilt to fit.", "short_accent": "built to fit.",
    "sub": "Onyx Engine serves one model per process. JX Runtime installs it, spawns it, health-checks it "
           "and puts scoped keys, TLS, pairing and an audit log in front of it.",
    "steps": [("Install", "jx-runtime backend install --engine onyx",
               "A prebuilt onyx-engine, checked against its release checksums, lands in the data directory."),
              ("Select", "execution.backend: \"onyxengine\"",
               "One setting. Its adapter subclasses the llama.cpp one: same spawn, streaming and restart code."),
              ("Serve", "one onyx-engine per loaded model",
               "Bound to 127.0.0.1, polled at /health, its OpenAI endpoints proxied through."),
              ("Share", "Authorization: Bearer jxr_…",
               "Apps and paired devices reach it through JX Runtime's keys, TLS and rate limits.")],
    "jx_jobs": ["Access keys & pairing", "TLS & network posture", "Model installs & fit checks",
                "Crash detection & restart", "Metrics without content", "Audit log"],
    "onyx_jobs": ["Inference on llama.cpp", "Continuous batching", "Streaming over SSE",
                  "Tool calls & grammars", "Embeddings", "Multimodal input"],
    "words": ["Install.", "Select.", "Serve.", "Share."],
}


def code_colors(b, lines):
    """Resolve the short colour codes used in COPY's terminal lines."""
    table = {None: b["accent"], "t": b["text"], "s": CODE_STR, "d": CODE_DIM, "k": CODE_KEY, "n": CODE_NUM}
    return [(indent, [(t, table[c]) for t, c in runs]) for indent, runs in lines]


# Shared furniture -------------------------------------------------------------------------------
def backdrop(b, W, H, seed=1, grid=True, glow_at=None, strength="40"):
    ops = []
    gx, gy, gs = glow_at or (W * 0.45, -H * 0.6, max(W, H) * 1.1)
    ops.append(glow("bd-glow", round(gx), round(gy), round(gs), b["accent"], strength))
    if b["key"] == "together":
        ops.append(glow("bd-glow2", round(W - gs * 0.5), round(H - gs * 0.45), round(gs * 0.9), b["teal"], "30"))
    if grid:
        if b["key"] == "onyx":
            ops += dot_grid("bd-grid", 24, 24, W - 24, H - 24, 34, "#FFFFFF0E")
        else:
            ops += scanlines("bd-lines", 0, 12, W, H - 12, 14, "#FFFFFF07")
    ops += decoration(*[o["name"] for o in ops if o["type"] == "gradient"])
    return ops


def stat_block(b, prefix, value, label, x, y, size, color=None, width=None):
    return [text(f"{prefix}-v", value, x, y, size, color or b["text"], b["head"]),
            para(f"{prefix}-l", label, x + 4, y + round(size * 1.12), round(size * 0.28), width or round(size * 3),
                 b["muted"], b["body"], line_height=1.2)]


def feature_grid(p, b, prefix, features, x, y, width, cols, size, row_h, light=False, card=False):
    fg, sub = (b["ink"], b["subtle"]) if light else (b["text"], b["muted"])
    gap = round(size * 1.6)
    cw = (width - gap * (cols - 1)) / cols
    ops = []
    for i, (t, d) in enumerate(features):
        cx, cy = round(x + (i % cols) * (cw + gap)), round(y + (i // cols) * row_h)
        if card:
            ops.append(rect(f"{prefix}-card{i}", cx, cy, cw, row_h - round(size * 0.9), b["panel"], radius=20,
                            stroke=b["line"], stroke_width=2))
            pad = round(size * 1.2)
        else:
            pad = 0
        ops += [rect(f"{prefix}-px{i}", cx + pad, cy + pad + round(size * 0.22), round(size * 0.55), round(size * 0.55),
                     b["accent"] if b["key"] != "together" else (b["accent"] if i % 2 == 0 else b["teal"])),
                text(f"{prefix}-t{i}", t, cx + pad + round(size * 1.15), cy + pad, size, fg, b["head"]),
                para(f"{prefix}-d{i}", d, cx + pad + round(size * 1.15), cy + pad + round(size * 1.55),
                     round(size * 0.72), round(cw - 2 * pad - size * 1.15), sub, b["body"], line_height=1.35)]
    p.apply(ops, detail="brief")


# Solo pieces (JX Runtime or Onyx Engine) ---------------------------------------------------------
def build_logos(kit):
    """Logo files: mark and lockups on dark, light and transparent grounds, as PNG and SVG."""
    b = kit.b
    for stem, (W, H), light, ground in [("mark-dark", (512, 512), False, b["bg"]),
                                        ("mark-transparent", (512, 512), False, "#00000000"),
                                        ("lockup-dark", (1400, 360), False, b["bg"]),
                                        ("lockup-light", (1400, 360), True, b["paper"]),
                                        ("lockup-transparent", (1400, 360), False, "#00000000")]:
        p = kit.new(W, H, ground)
        if stem.startswith("mark"):
            if b["key"] == "onyx":
                p.apply(onyx_mark("logo", 56, 56, 400), detail="brief")
            else:
                place_jx_mark(p, "logo", 56, 56, 400)
        else:
            lockup(p, b, "logo", 80, 100, 160, light=light)
            move_ink(p, "logo", cx=W / 2)
        kit.save(p, "logos", stem, [("png", {}), ("svg", {})], checks=["bounds", "fonts"])


def build_og(kit):
    """Open Graph / link card, 1200 x 630."""
    b, c = kit.b, COPY[kit.b["key"]]
    W, H = 1200, 630
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H), detail="brief")
    lockup(p, b, "logo", 64, 56, 64)
    three = c["hero"].count("\n") > 1
    p.apply(headline(b, "head", c["hero"], 64, 168, 60 if three else 76, c["accent"]), detail="brief")
    p.apply(para("sub", c["sub"], 64, bottom(p, "head") + 28, 21 if three else 24, 620, b["muted"], b["body"]),
            detail="brief")
    p.apply(code_card(b, "code", 690, 176, 470, code_colors(b, c["call"]), size=14, title="request"), detail="brief")
    fit_code(p, "code")
    p.apply({"type": "look", "target": "code", "look": "soft-shadow", "color": "#000000"}, detail="brief")
    _, cy, _, ch = bounds(p, "code-bg")
    p.apply(para("motto", c["motto"], 690, cy + ch + 28, 17, 420, b["accent2"], MONO), detail="brief")
    chips_row(p, b, "chip", c["chips"][:3], 64, H - 92, size=18)
    kit.save(p, "social", "og-card-1200x630", [("png", {})], thumbnail_width=600)


def build_github(kit):
    """GitHub social preview, 1280 x 640, light treatment."""
    b, c = kit.b, COPY[kit.b["key"]]
    W, H = 1280, 640
    p = kit.new(W, H, b["paper"])
    p.apply([*dot_grid("grid", 20, 20, W - 20, H - 20, 30, f"{b['ink']}14"),
             rect("panel", 760, 0, W - 760, H, b["bg"])], detail="brief")
    lockup(p, b, "logo", 64, 64, 60, light=True)
    p.apply(headline(b, "head", c["hero"], 64, 190, 70, c["accent"], color=b["ink"], accent_color=b["accent_deep"]
                     if b["key"] == "onyx" else b["accent"]), detail="brief")
    p.apply(para("sub", c["sub"], 64, bottom(p, "head") + 28, 22, 620, b["subtle"], b["body"]), detail="brief")
    if b["key"] == "onyx":
        p.apply(onyx_mark("big", 820, 70, 400), detail="brief")
    else:
        place_jx_mark(p, "big", 820, 70, 400)
    p.apply([glow("pglow", 700, -120, 700, b["accent"], "40"), *decoration("pglow"),
             {"type": "lower", "target": "pglow"}, {"type": "lower", "target": "panel"},
             {"type": "lower", "target": "grid-row"}], detail="brief")
    p.apply(text("repo", b["repo"], 0, H - 86, 20, b["muted"], MONO), detail="brief")
    move_ink(p, "repo", cx=760 + (W - 760) / 2)
    kit.save(p, "social", "github-preview-1280x640", [("png", {})], thumbnail_width=640)


def build_linkedin(kit):
    """LinkedIn banner, 1584 x 396; the left fifth stays quiet for the profile picture."""
    b, c = kit.b, COPY[kit.b["key"]]
    W, H = 1584, 396
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(800, -700, 1500)), detail="brief")
    p.apply(headline(b, "head", c["hero"], 460, 70 if c["hero"].count("\n") > 1 else 92, 54 if c["hero"].count("\n") < 2 else 44,
                     c["accent"], line_height=1.0), detail="brief")
    chips_row(p, b, "chip", c["chips"], 460, bottom(p, "head") + 34, size=18)
    lockup(p, b, "logo", W - 380, 64, 52)
    move_ink(p, "logo", rx=W - 64)
    p.apply(text("repo", b["repo"], 0, H - 80, 18, b["muted"], MONO), detail="brief")
    move_ink(p, "repo", rx=W - 64)
    kit.save(p, "social", "linkedin-banner-1584x396", [("png", {})], thumbnail_width=792)


def build_xheader(kit):
    """X / Twitter header, 1500 x 500; the bottom-left corner stays clear for the avatar."""
    b, c = kit.b, COPY[kit.b["key"]]
    W, H = 1500, 500
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(600, -600, 1400)), detail="brief")
    if b["key"] == "onyx":
        p.apply(onyx_mark("big", W - 470, 60, 380), detail="brief")
    else:
        place_jx_mark(p, "big", W - 450, 60, 360)
    p.apply([headline(b, "head", c["hero_long"], 80, 70, 52, c["hero_long_accent"], line_height=1.02),
             text("motto", c["motto"], 80, 0, 20, b["muted"], MONO)], detail="brief")
    move_ink(p, "motto", y=bottom(p, "head") + 30)
    kit.save(p, "social", "x-header-1500x500", [("png", {})], thumbnail_width=750)


def build_story(kit):
    """Vertical story, 1080 x 1920; top and bottom 250 px clear of platform UI."""
    b, c = kit.b, COPY[kit.b["key"]]
    W, H, M = 1080, 1920, 84
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(-300, 700, 1700), strength="50"), detail="brief")
    lockup(p, b, "logo", M, 280, 72)
    p.apply(headline(b, "head", c["hero"], M, 430, 112, c["accent"], line_height=0.98), detail="brief")
    p.apply(para("sub", c["sub"], M, bottom(p, "head") + 48, 38, W - 2 * M, b["muted"], b["body"]), detail="brief")
    top = bottom(p, "sub") + 60
    feats = c["features"] if c["hero"].count("\n") < 2 else c["features"][:4]
    ops = []
    for i, (t, d) in enumerate(feats):
        y = top + i * 112
        ops += [rect(f"f-bg{i}", M, y, W - 2 * M, 92, b["panel"], radius=20, stroke=b["line"], stroke_width=2),
                rect(f"f-px{i}", M + 32, y + 36, 20, 20, b["accent"]),
                text(f"f-t{i}", t, M + 78, y + 24, 38, b["text"], b["head"])]
    p.apply(ops, detail="brief")
    for i in range(len(feats)):
        move_ink(p, f"f-t{i}", cy=top + i * 112 + 46)
    p.apply(text("repo", b["repo"], M, top + len(feats) * 112 + 40, 30, b["accent2"], MONO), detail="brief")
    kit.save(p, "social", "story-1080x1920", [("png", {})], thumbnail_width=540,
             safe_area={"left": 60, "right": 60, "top": 250, "bottom": 250})


def build_squares(kit):
    """Three 1080 x 1080 feed posts: a statement, a feature, and a get-started card."""
    b, c = kit.b, COPY[kit.b["key"]]
    W = H = 1080
    M = 88
    # 1 statement
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(-200, -300, 1400), strength="55"), detail="brief")
    p.apply([text("kicker", c["kicker"], M, 120, 22, b["accent"], MONO),
             headline(b, "head", c["hero"], M, 260, 112, c["accent"])], detail="brief")
    p.apply(para("motto", c["motto"], M, bottom(p, "head") + 56, 34, 600, b["muted"], b["body"]), detail="brief")
    if b["key"] == "onyx":
        p.apply(onyx_mark("big", W - 400, H - 420, 340), detail="brief")
    else:
        place_jx_mark(p, "big", W - 380, H - 400, 300)
    lockup(p, b, "logo", M, H - 160, 64)
    kit.save(p, "social", "post-1-statement-1080", [("png", {})], thumbnail_width=540)
    # 2 features
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(500, -500, 1300)), detail="brief")
    p.apply([text("kicker", "WHAT SHIPS", M, 110, 22, b["accent"], MONO),
             headline(b, "head", "Six things,\non by default." if b["key"] == "jx" else "Six things\nit does well.",
                      M, 160, 84, "on by default." if b["key"] == "jx" else "does well.")], detail="brief")
    feature_grid(p, b, "f", c["features"], M, bottom(p, "head") + 60, W - 2 * M, 2, 32, 210)
    lockup(p, b, "logo", M, H - 120, 48)
    kit.save(p, "social", "post-2-features-1080", [("png", {})], thumbnail_width=540)
    # 3 get started
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(100, 400, 1300), strength="50"), detail="brief")
    p.apply([text("kicker", "GET STARTED", M, 110, 22, b["accent"], MONO),
             headline(b, "head", "Two commands\nto a served model.", M, 160, 84, "a served model.")],
            detail="brief")
    p.apply(code_card(b, "code", M, bottom(p, "head") + 64, W - 2 * M, code_colors(b, c["quick"]), size=27),
            detail="brief")
    fit_code(p, "code")
    p.apply(code_card(b, "call", M, bottom(p, "code-bg") + 32, W - 2 * M, code_colors(b, c["call"]), size=21,
                      title="then, from any OpenAI client"), detail="brief")
    fit_code(p, "call")
    lockup(p, b, "logo", M, H - 120, 48)
    p.apply(text("repo", b["repo"], 0, H - 112, 22, b["muted"], MONO), detail="brief")
    move_ink(p, "repo", rx=W - M, cy=H - 96)
    kit.save(p, "social", "post-3-get-started-1080", [("png", {})], thumbnail_width=540)


def build_carousel(kit):
    """Six-slide carousel, 1080 x 1350: PDF, one PNG per slide, contact sheet."""
    b, c = kit.b, COPY[kit.b["key"]]
    W, H, M = 1080, 1350, 88
    p = kit.new(W, H)
    p.apply([{"type": "master", "action": "add", "name": "frame", "background": b["bg"]},
             *(dot_grid("m-grid", 24, 24, W - 24, H - 24, 36, "#FFFFFF0C") if b["key"] == "onyx"
               else scanlines("m-lines", 0, 12, W, H - 12, 14, "#FFFFFF07")),
             text("m-num", "${page} / ${pages}", W - M - 80, H - 100, 24, b["muted"], MONO)], detail="brief")
    lockup(p, b, "m-logo", M, H - 118, 44)

    def page(name):
        p.apply({"type": "page", "action": "add", "name": name, "master": "frame"}, detail="brief")

    page("cover")
    p.apply([glow("c-glow", -300, -300, 1500, b["accent"], "50"), *decoration("c-glow"),
             text("c-kicker", c["kicker"], M, 170, 22, b["accent"], MONO),
             headline(b, "c-title", c["hero"], M, 430, 132, c["accent"])], detail="brief")
    p.apply(para("c-sub", c["sub"], M, bottom(p, "c-title") + 48, 34, W - 2 * M, b["muted"], b["body"]),
            detail="brief")

    page("problem")
    p.apply([text("p-kicker", "THE PROBLEM", M, 150, 24, b["accent"], MONO),
             headline(b, "p-title", c["problem_head"], M, 220, 90, c["problem_accent"])], detail="brief")
    y = bottom(p, "p-title") + 80
    ops = []
    for i, (t, d) in enumerate(c["problems"]):
        yy = y + i * 190
        ops += [rect(f"p-px{i}", M, yy + 12, 22, 22, b["accent"]),
                text(f"p-t{i}", t, M + 56, yy, 44, b["text"], b["head"]),
                para(f"p-d{i}", d, M + 56, yy + 64, 32, W - 2 * M - 56, b["muted"], b["body"])]
    p.apply(ops, detail="brief")

    for half, name in ((0, "features-1"), (1, "features-2")):
        page(name)
        head, accent = c["feature_titles"][half]
        p.apply([text(f"{name}-kicker", "WHAT SHIPS" if half == 0 else "AND ALSO", M, 150, 24, b["accent"], MONO),
                 headline(b, f"{name}-title", head, M, 220, 90, accent)], detail="brief")
        feature_grid(p, b, f"{name}-f", c["features"][half * 3:half * 3 + 3], M, bottom(p, f"{name}-title") + 70,
                     W - 2 * M, 1, 40, 230, card=True)

    page("numbers")
    p.apply([text("n-kicker", "BY THE NUMBERS", M, 150, 24, b["accent"], MONO),
             headline(b, "n-title", "Small on\npurpose.", M, 220, 90, "on\npurpose.")], detail="brief")
    top = bottom(p, "n-title") + 80
    ops = []
    for i, (v, label) in enumerate(c["stats"]):
        ops += stat_block(b, f"n-{i}", v, label, M + (i % 2) * 460, top + (i // 2) * 250, 130,
                          b["accent"] if i in (0, 3) else b["text"], width=400)
    p.apply(ops, detail="brief")
    p.apply(para("n-note", c["privacy"], M, top + 540, 30, W - 2 * M, b["accent2"], b["semi"]), detail="brief")

    page("start")
    p.apply([glow("s-glow", 0, 300, 1300, b["accent"], "55"), *decoration("s-glow"),
             text("s-kicker", "GET STARTED", M, 150, 24, b["accent"], MONO),
             headline(b, "s-title", "Two commands\nto a served model.", M, 220, 90, "a served model.")], detail="brief")
    p.apply(code_card(b, "s-code", M, bottom(p, "s-title") + 70, W - 2 * M, code_colors(b, c["quick"]), size=27),
            detail="brief")
    fit_code(p, "s-code")
    p.apply([para("s-status", c["status"], M, bottom(p, "s-code-bg") + 50, 28, W - 2 * M, b["muted"], b["body"]),
             text("s-url", b["repo"], M, 0, 32, b["text"], MONO)], detail="brief")
    move_ink(p, "s-url", y=bottom(p, "s-status") + 50)

    names = ["cover", "problem", "features-1", "features-2", "numbers", "start"]
    kit.save(p, "carousel", "carousel-1080x1350", [("pdf", {})], thumbnail_width=540,
             checks=["bounds", "overlap", "contrast", "fonts", "deck"], deck={"profile": "phone"})
    for i, name in enumerate(names, 1):
        p.export(str(kit.out / "carousel" / f"slide-{i}-{name}.png"), page=name, overwrite=True)
    kit.sheet(p, "carousel", "contact-sheet.png", width=360, columns=6)


def deck_base(kit):
    b = kit.b
    W, H, M = 1920, 1080, 140
    p = kit.new(W, H)
    p.apply([{"type": "master", "action": "add", "name": "std", "background": b["bg"]},
             *(dot_grid("m-grid", 30, 30, W - 30, H - 30, 40, "#FFFFFF0A") if b["key"] == "onyx"
               else scanlines("m-lines", 0, 12, W, H - 12, 16, "#FFFFFF06")),
             text("m-num", "${page}", W - M - 30, H - 96, 24, b["muted"], MONO)], detail="brief")
    if b["key"] == "together":
        joint_lockup(p, "m-logo", M, H - 118, 40)
    else:
        lockup(p, b, "m-logo", M, H - 118, 40)
    return p, W, H, M


def deck_slide(p, name, notes, master="std"):
    p.apply({"type": "page", "action": "add", "name": name, "master": master, "notes": notes, "transition": "fade"},
            detail="brief")


def deck_title(p, b, prefix, kicker, head, accent=None, y=170, size=96, M=140):
    p.apply([text(f"{prefix}-kicker", kicker, M, y - 56, 24, b["accent"], MONO),
             headline(b, f"{prefix}-title", head, M, y, size, accent)], detail="brief")
    return bottom(p, f"{prefix}-title")


def build_deck(kit):
    """Eight-slide pitch deck, 1920 x 1080: PDF, editable PPTX, HTML presenter."""
    b, c = kit.b, COPY[kit.b["key"]]
    p, W, H, M = deck_base(kit)

    deck_slide(p, "title", f"{b['name']}: {c['motto']}", master=None)
    p.apply(backdrop(b, W, H, glow_at=(700, -500, 1900), strength="50"), detail="brief")
    lockup(p, b, "t-logo", M, 150, 90)
    if b["key"] == "onyx":
        p.apply(onyx_mark("t-big", W - 700, 220, 560), detail="brief")
    else:
        place_jx_mark(p, "t-big", W - 680, 240, 520)
    p.apply(headline(b, "t-title", c["hero"], M, 380, 120, c["accent"]), detail="brief")
    p.apply(text("t-sub", c["motto"], M, bottom(p, "t-title") + 60, 30, b["muted"], MONO), detail="brief")

    deck_slide(p, "problem", "The gap this project closes.")
    y = deck_title(p, b, "pr", "THE PROBLEM", c["problem_deck"], c["problem_deck_accent"])
    cw = (W - 2 * M - 80) / 3
    ops = []
    for i, (t, d) in enumerate(c["problems"]):
        cx = round(M + i * (cw + 40))
        ops += [rect(f"pr-card{i}", cx, y + 70, cw, 340, b["panel"], radius=24, stroke=b["line"], stroke_width=2),
                text(f"pr-n{i}", f"0{i + 1}", cx + 44, y + 112, 30, b["accent"], b["head"]),
                text(f"pr-t{i}", t, cx + 44, y + 170, 46, b["text"], b["head"]),
                para(f"pr-d{i}", d, cx + 44, y + 250, 30, round(cw - 88), b["muted"], b["body"])]
    p.apply(ops, detail="brief")

    deck_slide(p, "features", "Six capabilities, each from the README.")
    y = deck_title(p, b, "fe", "WHAT SHIPS", "Six things, built in.", "built in.")
    feature_grid(p, b, "fe-f", c["features"], M, y + 60, W - 2 * M, 3, 40, 340, card=True)

    if b["key"] == "jx":
        deck_slide(p, "postures", "Connectivity modes name the posture rather than a bind address.")
        y = deck_title(p, b, "po", "NETWORK POSTURE", "Say who can reach it.", "who can reach it.")
        modes = [("local", "This machine only. The default.", "127.0.0.1"),
                 ("lan", "Your local network, keys required.", "LAN"),
                 ("private-network", "A WireGuard or Tailscale network you run.", "VPN"),
                 ("tunnel", "Through a tunnel, auth always required.", "TUNNEL")]
        cw = (W - 2 * M - 3 * 30) / 4
        ops = []
        for i, (mname, d, tag) in enumerate(modes):
            cx = round(M + i * (cw + 30))
            ops += [rect(f"po-card{i}", cx, y + 80, cw, 380, b["panel"], radius=24, stroke=b["line"], stroke_width=2),
                    rect(f"po-bar{i}", cx, y + 80, cw * (i + 1) / 4, 8, b["accent"]),
                    text(f"po-tag{i}", tag, cx + 40, y + 130, 22, b["accent2"], MONO),
                    text(f"po-n{i}", mname, cx + 40, y + 180, 46 if len(mname) < 10 else 38, b["text"], b["head"]),
                    para(f"po-d{i}", d, cx + 40, y + 270, 28, round(cw - 80), b["muted"], b["body"])]
        p.apply(ops, detail="brief")
        p.apply(para("po-note", "Widening exposure is refused when it would be unsafe: no auth.mode off outside local, "
                     "no tunnel without required auth. The security panel says what is open.", M, y + 520, 28,
                     W - 2 * M, b["muted"], b["body"]), detail="brief")
    else:
        deck_slide(p, "endpoints", "The full HTTP surface; docs/api.md has every field.")
        y = deck_title(p, b, "ep", "ENDPOINTS", "Nine routes. That's the API.", "That's the API.")
        ops = []
        cw = (W - 2 * M - 40) / 3
        for i, (verb, path) in enumerate(ENDPOINTS):
            cx, cy = round(M + (i % 3) * (cw + 20)), y + 80 + (i // 3) * 130
            ops += [rect(f"ep-c{i}", cx, cy, cw, 106, b["panel"], radius=18, stroke=b["line"], stroke_width=2),
                    text(f"ep-v{i}", verb, cx + 32, cy + 38, 24, b["accent"] if verb == "POST" else b["accent2"], MONO),
                    text(f"ep-p{i}", path, cx + 120, cy + 34, 30, b["text"], MONO)]
        p.apply(ops, detail="brief")
        p.apply(para("ep-note", "SSE streaming ends in a frame carrying usage and timings. /health is exempt from "
                     "--api-key, so a supervisor can poll it.", M, y + 500, 28, W - 2 * M, b["muted"], b["body"]),
                detail="brief")

    deck_slide(p, "numbers", "Counts taken from the README.")
    y = deck_title(p, b, "nu", "BY THE NUMBERS", "Small on purpose.", "on purpose.")
    ops = []
    cw = (W - 2 * M) / 4
    for i, (v, label) in enumerate(c["stats"]):
        ops += stat_block(b, f"nu-{i}", v, label, round(M + i * cw), y + 110, 180,
                          b["accent"] if i in (0, 3) else b["text"], width=round(cw - 60))
    p.apply(ops, detail="brief")
    p.apply(para("nu-note", c["privacy"], M, y + 470, 36, W - 2 * M, b["accent2"], b["semi"]), detail="brief")

    deck_slide(p, "use", "Any OpenAI-compatible client works unchanged.")
    y = deck_title(p, b, "us", "USE IT", "Point any OpenAI client at it.", "any OpenAI client")
    p.apply(code_card(b, "us-call", M, y + 80, W - 2 * M, code_colors(b, c["call"]), size=32, title="request"),
            detail="brief")
    fit_code(p, "us-call")
    p.apply(text("us-h", "Speaks the routes your client already calls:", M, bottom(p, "us-call-bg") + 70, 30,
                 b["muted"], b["body"]), detail="brief")
    chips_row(p, b, "us-r", ["/v1/models", "/v1/chat/completions", "/v1/completions", "/v1/embeddings", "SSE streaming"],
              M, bottom(p, "us-h") + 30, size=28, font=MONO, gap=16)

    deck_slide(p, "pairing" if b["key"] == "jx" else "hardware", "")
    if b["key"] == "jx":
        y = deck_title(p, b, "pa", "DEVICE PAIRING", "A code, not a config file.", "not a config file.")
        p.apply([rect("pa-card", M, y + 80, 760, 380, b["panel"], radius=28, stroke=b["line"], stroke_width=2),
                 text("pa-lbl", "PAIRING CODE", M + 60, y + 130, 22, b["muted"], MONO),
                 text("pa-code", "JX-7KQ4-M2TD", M + 60, y + 190, 84, b["text"], MONO),
                 text("pa-exp", "single use  ·  stored hashed  ·  expires", M + 60, y + 330, 24, b["accent2"], MONO)],
                detail="brief")
        steps = [("1", "Generate a code in the UI or with POST /runtime/pairing."),
                 ("2", "The device redeems it at POST /runtime/pairing/claim."),
                 ("3", "It gets the endpoint, a freshly minted scoped key and the certificate fingerprint."),
                 ("4", "List or revoke paired devices at any time.")]
        ops = []
        for i, (n, d) in enumerate(steps):
            yy = y + 90 + i * 96
            ops += [text(f"pa-n{i}", n, M + 840, yy, 44, b["accent"], b["head"]),
                    para(f"pa-d{i}", d, M + 910, yy + 6, 30, W - 2 * M - 910, b["text"], b["body"])]
        p.apply(ops, detail="brief")
    else:
        y = deck_title(p, b, "hw", "BUILD TARGETS", "CPU by default. GPU when built for it.", "GPU when built for it.")
        targets = ["CPU", "CUDA", "Vulkan", "Metal", "ROCm / HIP", "BLAS"]
        cw = (W - 2 * M - 5 * 24) / 6
        ops = []
        for i, t in enumerate(targets):
            cx = round(M + i * (cw + 24))
            ops += [rect(f"hw-c{i}", cx, y + 90, cw, 180, b["accent"] if i == 0 else b["panel"], radius=22,
                         stroke=b["line"], stroke_width=2),
                    text(f"hw-t{i}", t, cx + 30, y + 160, 40, b["ink"] if i == 0 else b["text"], b["head"])]
        p.apply(ops, detail="brief")
        plats = ["linux-x64", "darwin-arm64", "darwin-x64", "win32-x64"]
        p.apply([text("hw-ph", "Prebuilt release archives, with checksums.txt:", M, y + 340, 30, b["muted"], b["body"])],
                detail="brief")
        chips_row(p, b, "hw-p", plats, M, y + 400, size=30, font=MONO, fg=b["text"])

    deck_slide(p, "start", "Install and go.", master=None)
    p.apply(backdrop(b, W, H, glow_at=(-300, 200, 1800), strength="55"), detail="brief")
    p.apply(headline(b, "st-title", "Two commands\nto a served model.", M, 180, 110, "a served model."), detail="brief")
    p.apply(code_card(b, "st-code", M, bottom(p, "st-title") + 70, 1300, code_colors(b, c["quick"]), size=30),
            detail="brief")
    fit_code(p, "st-code")
    p.apply(text("st-status", c["status"], M, bottom(p, "st-code-bg") + 50, 26, b["muted"], b["body"]), detail="brief")
    lockup(p, b, "st-logo", M, H - 190, 70)
    p.apply(text("st-url", b["repo"], 0, H - 170, 34, b["text"], MONO), detail="brief")
    move_ink(p, "st-url", rx=W - M, cy=H - 155)

    stem = f"{b['key'] if b['key'] != 'jx' else 'jx-runtime'}-pitch-deck".replace("onyx-", "onyx-engine-")
    kit.save(p, "deck", stem, [("pdf", {}), ("pptx", {}), ("html", {})],
             checks=["bounds", "overlap", "contrast", "fonts", "deck"], deck={"profile": "screen"})
    kit.sheet(p, "deck", "contact-sheet.png", width=480, columns=4)
    p.export(str(kit.out / "deck" / "deck-title.png"), page="title", scale=0.5, overwrite=True)


def build_onepager(kit):
    """US Letter product sheet, light, vector PDF plus PNG."""
    b, c = kit.b, COPY[kit.b["key"]]
    p = kit.new(100, 100, "#FFFFFF")
    p.apply({"type": "canvas", "size": "letter", "dpi": 150, "background": "#FFFFFF"}, detail="brief")
    W, H = p.state["canvas"]["width"], p.state["canvas"]["height"]
    M = 90
    p.apply([rect("band", 0, 0, W, 560, b["bg"]), glow("glow", 520, -520, 1200, b["accent"], "50"),
             *decoration("glow")], detail="brief")
    lockup(p, b, "logo", M, 70, 56)
    p.apply([text("kicker", "PRODUCT SHEET", 0, 88, 18, b["accent2"], MONO),
             headline(b, "head", c["hero_long"], M, 200, 56, c["hero_long_accent"], line_height=1.02)], detail="brief")
    move_ink(p, "kicker", rx=W - M, cy=98)
    p.apply(para("sub", c["sub"], M, bottom(p, "head") + 28, 23, W - 2 * M - 60, b["muted"], b["body"]), detail="brief")
    p.apply({"type": "resize", "target": "band", "height": round(bottom(p, "sub") + 60)}, detail="brief")
    top = bottom(p, "band") + 50
    p.apply(text("fh", "What ships", M, top, 30, b["ink"], b["head"]), detail="brief")
    feature_grid(p, b, "f", c["features"], M, bottom(p, "fh") + 30, W - 2 * M, 2, 27, 138, light=True)
    top = max(bottom(p, "f-d4"), bottom(p, "f-d5")) + 40
    p.apply(rect("stats-bg", M, top, W - 2 * M, 150, b["paper"], radius=18), detail="brief")
    sw = (W - 2 * M) / 4
    ops = []
    for i, (v, label) in enumerate(c["stats"]):
        ops += [text(f"st-v{i}", v, round(M + 30 + i * sw), top + 18, 60, b["accent"] if b["key"] != "onyx" else b["accent_deep"],
                     b["head"]),
                para(f"st-l{i}", label, round(M + 30 + i * sw), top + 96, 18, round(sw - 40), b["subtle"], b["body"],
                     line_height=1.2)]
    p.apply(ops, detail="brief")
    top = top + 150 + 34
    p.apply(text("qh", "Get started", M, top, 26, b["ink"], b["head"]), detail="brief")
    p.apply(code_card(b, "q", M, bottom(p, "qh") + 18, W - 2 * M, code_colors(b, c["quick"] + c["call"]), size=19,
                      chrome=False), detail="brief")
    fit_code(p, "q", 22)
    p.apply([rect("foot-rule", M, H - 120, W - 2 * M, 2, "#DCDAE0"),
             text("foot-status", c["status"], M, H - 92, 17, b["subtle"], b["body"]),
             text("foot-url", b["repo"], 0, H - 92, 18, b["ink"], MONO)], detail="brief")
    move_ink(p, "foot-url", rx=W - M)
    kit.save(p, "print", "one-pager-letter", [("pdf", {}), ("png", {"scale": 0.6})],
             checks=["bounds", "overlap", "contrast", "fonts"])


def build_teaser(kit):
    """Six-second square motion teaser: MP4, GIF, poster frame and contact sheet."""
    b, c = kit.b, COPY[kit.b["key"]]
    W = H = 1080
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(-200, -200, 1500), strength="45"), detail="brief")
    if b["key"] == "onyx":
        p.apply(onyx_mark("mark", 365, 230, 350), detail="brief")
    else:
        place_jx_mark(p, "mark", 365, 230, 350)
    words = c["words"]
    p.apply([headline(b, f"w{i}", w, 0, 700, 110, w if i == len(words) - 1 else None, align="center", width=W)
             for i, w in enumerate(words)], detail="brief")
    lockup(p, b, "end-logo", 0, 380, 110)
    move_ink(p, "end-logo", cx=W / 2)
    p.apply([text("end-line", c["end"], 0, 600, 46, b["text"], b["semi"]),
             text("end-url", b["repo"], 0, 690, 30, b["accent2"], MONO),
             {"type": "timeline-set", "duration": 6600, "fps": 24}], detail="brief")
    move_ink(p, "end-line", cx=W / 2)
    move_ink(p, "end-url", cx=W / 2)
    teaser_motion(p, len(words))
    write_motion(kit, p, "teaser")


def teaser_motion(p, n, word_start=500, step=None, end_at=4300):
    step = step or round((end_at - word_start) / n)
    ops = [{"type": "keyframe", "target": "mark", "property": "scale", "time": 0, "value": 0.92, "easing": "ease-out-cubic"},
           {"type": "keyframe", "target": "mark", "property": "scale", "time": 700, "value": 1},
           {"type": "keyframe", "target": "mark", "property": "opacity", "time": end_at - 300, "value": 1},
           {"type": "keyframe", "target": "mark", "property": "opacity", "time": end_at, "value": 0}]
    for i in range(n):
        t0 = word_start + i * step
        ops += [{"type": "keyframe", "target": f"w{i}", "property": "opacity", "time": 0, "value": 0, "easing": "hold"},
                {"type": "keyframe", "target": f"w{i}", "property": "opacity", "time": t0, "value": 0},
                {"type": "keyframe", "target": f"w{i}", "property": "opacity", "time": t0 + 160, "value": 1},
                {"type": "keyframe", "target": f"w{i}", "property": "translate-y", "time": t0, "value": 40,
                 "easing": "ease-out-cubic"},
                {"type": "keyframe", "target": f"w{i}", "property": "translate-y", "time": t0 + 260, "value": 0},
                {"type": "keyframe", "target": f"w{i}", "property": "opacity", "time": t0 + step - 120, "value": 1},
                {"type": "keyframe", "target": f"w{i}", "property": "opacity", "time": t0 + step, "value": 0}]
    for name, t in (("end-logo", end_at), ("end-line", end_at + 300), ("end-url", end_at + 550)):
        ops += [{"type": "keyframe", "target": name, "property": "opacity", "time": 0, "value": 0, "easing": "hold"},
                {"type": "animate-preset", "target": name, "preset": "slide-in-up", "start": t, "duration": 500,
                 "distance": 40, "fade": True}]
    p.apply(ops, detail="brief")


def write_motion(kit, p, stem):
    from vixl.timeline import contact_sheet, export_timeline, render_at

    folder = kit.out / "motion"
    folder.mkdir(parents=True, exist_ok=True)
    p.save(str(folder / f"{stem}.vixl"), overwrite=True)
    export_timeline(p, str(folder / f"{stem}.mp4"), fps=24, overwrite=True)
    export_timeline(p, str(folder / f"{stem}.gif"), fps=12, scale=0.5, colors=96, overwrite=True)
    render_at(p, 6200).convert("RGB").save(folder / f"{stem}-frame.png")
    contact_sheet(p, count=12, columns=6, max_width=1800).convert("RGB").save(folder / f"{stem}-contact-sheet.png")
    print(f"  wrote motion/{stem}.{{vixl,mp4,gif}} and contact sheet")


# Joint pieces ------------------------------------------------------------------------------------
def architecture(p, b, prefix, x, y, w, h, size=1.0):
    """Clients -> JX Runtime -> one onyx-engine per model, drawn with live shapes."""
    s = size
    col_client = round(w * 0.2)
    col_jx = round(w * 0.36)
    col_onyx = round(w * 0.26)
    gap = (w - col_client - col_jx - col_onyx) / 2
    xc, xj, xo = x, round(x + col_client + gap), round(x + col_client + gap + col_jx + gap)
    ops = []
    clients = ["Your app", "OpenAI SDK", "Paired phone"]
    ch = round((h - 2 * 24 * s) / 3)
    for i, label in enumerate(clients):
        cy = round(y + i * (ch + 24 * s))
        ops += [rect(f"{prefix}-cl{i}", xc, cy, col_client, ch, b["panel"], radius=round(18 * s), stroke=b["line"],
                     stroke_width=2),
                text(f"{prefix}-cl{i}-t", label, xc + round(24 * s), cy + round(ch / 2 - 18 * s), round(26 * s),
                     b["text"], b["semi"])]
    # JX Runtime box
    ops += [rect(f"{prefix}-jx", xj, y, col_jx, h, b["panel"], radius=round(26 * s), stroke=JX["accent"],
                 stroke_width=3),
            text(f"{prefix}-jx-k", "JX RUNTIME  :8712", xj + round(32 * s), y + round(30 * s), round(20 * s),
                 JX["accent"], MONO)]
    layers = ["Scoped keys & pairing", "TLS · network posture", "Rate limits · audit log", "Fit check · Auto config",
              "Spawn · /health · restart"]
    lh = round((h - 100 * s) / len(layers))
    for i, label in enumerate(layers):
        ly = round(y + 82 * s + i * lh)
        ops += [rect(f"{prefix}-jl{i}", xj + round(28 * s), ly, col_jx - round(56 * s), lh - round(14 * s),
                     b["bg"], radius=round(12 * s), stroke=b["line"], stroke_width=1),
                text(f"{prefix}-jl{i}-t", label, xj + round(52 * s), ly + round((lh - 14 * s) / 2 - 15 * s),
                     round(24 * s), b["text"], b["body"])]
    # Onyx processes
    models = [("onyx-engine", "model A · 127.0.0.1"), ("onyx-engine", "model B · 127.0.0.1"),
              ("onyx-engine", "embeddings · 127.0.0.1")]
    for i, (name, sub) in enumerate(models):
        cy = round(y + i * (ch + 24 * s))
        ops += [rect(f"{prefix}-ox{i}", xo, cy, col_onyx, ch, b["panel"], radius=round(18 * s), stroke=ONYX["accent"],
                     stroke_width=2),
                text(f"{prefix}-ox{i}-t", name, xo + round(80 * s), cy + round(ch / 2 - 30 * s), round(26 * s),
                     ONYX["text"], ONYX["head"]),
                text(f"{prefix}-ox{i}-s", sub, xo + round(80 * s), cy + round(ch / 2 + 6 * s), round(17 * s),
                     ONYX["muted"], MONO)]
    p.apply(ops, detail="brief")
    for i in range(3):
        cy = round(y + i * (ch + 24 * s))
        p.apply(onyx_mark(f"{prefix}-oxm{i}", xo + round(20 * s), cy + round(ch / 2 - 24 * s), round(48 * s)),
                detail="brief")
        # connectors: client -> JX, JX -> engine
        p.apply([{"type": "shape", "shape": "arrow", "name": f"{prefix}-a{i}", "x": xc + col_client + 8,
                  "y": cy + round(ch / 2 - 9 * s), "width": round(gap - 16), "height": round(18 * s),
                  "fill": JX["accent"], "shaft_width": max(2, round(4 * s))},
                 {"type": "shape", "shape": "arrow", "name": f"{prefix}-b{i}", "x": xj + col_jx + 8,
                  "y": cy + round(ch / 2 - 9 * s), "width": round(gap - 16), "height": round(18 * s),
                  "fill": ONYX["accent"], "shaft_width": max(2, round(4 * s))}], detail="brief")
    for i in range(3):
        move_ink(p, f"{prefix}-cl{i}-t", cx=xc + col_client / 2)
    p.apply([text(f"{prefix}-lab1", "OpenAI API + jxr_ key", xc + col_client + 4, y - round(40 * s), round(17 * s),
                  b["muted"], MONO),
             text(f"{prefix}-lab2", "one process per model", xj + col_jx + 4, y - round(40 * s), round(17 * s),
                  b["muted"], MONO)], detail="brief")
    move_ink(p, f"{prefix}-lab1", cx=xc + col_client + gap / 2)
    move_ink(p, f"{prefix}-lab2", cx=xj + col_jx + gap / 2)


def build_joint_og(kit):
    b = kit.b
    W, H = 1200, 630
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(-300, -500, 1300), strength="45"), detail="brief")
    joint_lockup(p, "logo", 64, 56, 48)
    p.apply(headline(b, "head", JOINT["hero"], 64, 176, 58, JOINT["accent"], line_height=1.04), detail="brief")
    p.apply(link_line("link", 64, bottom(p, "head") + 34, 180, 6), detail="brief")
    p.apply(para("sub", JOINT["sub"], 64, bottom(p, "head") + 66, 22, 640, b["muted"], b["body"]), detail="brief")
    p.apply(onyx_mark("gem", 880, 270, 240), detail="brief")
    place_jx_mark(p, "tile", 800, 170, 170)
    p.apply({"type": "look", "target": "tile", "look": "soft-shadow", "color": "#000000", "amount": 0.6},
            detail="brief")
    kit.save(p, "social", "og-card-1200x630", [("png", {})], thumbnail_width=600)


def build_joint_github(kit):
    b = kit.b
    W, H = 1280, 640
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(-200, -400, 1200), strength="40"), detail="brief")
    joint_lockup(p, "logo", 64, 60, 50)
    p.apply(headline(b, "head", JOINT["hero_short"], 64, 180, 72, JOINT["short_accent"], accent_color=b["teal"]),
            detail="brief")
    p.apply(link_line("link", 64, bottom(p, "head") + 36, 220, 6), detail="brief")
    p.apply(para("sub", "A local model runtime and the single-model engine it spawns: one OpenAI-compatible "
                 "endpoint with keys, TLS and an audit log in front.", 64, bottom(p, "head") + 70, 22, 600,
                 b["muted"], b["body"]), detail="brief")
    architecture(p, b, "arch", 720, 180, 490, 320, size=0.54)
    kit.save(p, "social", "github-preview-1280x640", [("png", {})], thumbnail_width=640)


def build_joint_linkedin(kit):
    b = kit.b
    W, H = 1584, 396
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(700, -700, 1500)), detail="brief")
    p.apply(headline(b, "head", JOINT["hero"], 460, 90, 52, JOINT["accent"], line_height=1.04), detail="brief")
    p.apply(link_line("link", 460, bottom(p, "head") + 34, 240, 6), detail="brief")
    p.apply(text("sub", "Local, OpenAI-compatible, safe to share.", 460, bottom(p, "head") + 64, 24, b["muted"],
                 b["body"]), detail="brief")
    place_jx_mark(p, "tile", W - 380, 110, 150)
    p.apply(onyx_mark("gem", W - 210, 110, 150), detail="brief")
    kit.save(p, "social", "linkedin-banner-1584x396", [("png", {})], thumbnail_width=792)


def build_joint_xheader(kit):
    b = kit.b
    W, H = 1500, 500
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(500, -600, 1400)), detail="brief")
    joint_lockup(p, "logo", 80, 80, 56)
    p.apply(headline(b, "head", "The engine and the runtime,\nbuilt to fit.", 80, 200, 60, "built to fit.",
                     accent_color=b["teal"], line_height=1.02), detail="brief")
    p.apply(link_line("link", 80, bottom(p, "head") + 34, 260, 6), detail="brief")
    kit.save(p, "social", "x-header-1500x500", [("png", {})], thumbnail_width=750)


def build_joint_story(kit):
    b = kit.b
    W, H, M = 1080, 1920, 84
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(-400, 300, 1600), strength="45"), detail="brief")
    place_jx_mark(p, "tile", M, 280, 120)
    p.apply([text("plus", "+", M + 150, 300, 80, b["muted"], b["semi"]), *onyx_mark("gem", M + 230, 280, 120)],
            detail="brief")
    p.apply(headline(b, "head", JOINT["hero_tall"], M, 470, 132, JOINT["tall_accent"], line_height=0.96), detail="brief")
    p.apply(link_line("link", M, bottom(p, "head") + 50, 300, 8), detail="brief")
    top = bottom(p, "head") + 110
    ops = []
    for i, (step, cmd, _) in enumerate(JOINT["steps"]):
        y = top + i * 104
        color = JX["accent"] if i in (0, 3) else ONYX["accent"]
        ops += [rect(f"s-bg{i}", M, y, W - 2 * M, 86, b["panel"], radius=18, stroke=b["line"], stroke_width=2),
                text(f"s-n{i}", f"0{i + 1}", M + 30, y + 22, 34, color, b["head"]),
                text(f"s-t{i}", step, M + 110, y + 22, 34, b["text"], b["head"]),
                text(f"s-c{i}", cmd, M + 330, y + 30, 18, b["muted"], MONO)]
    p.apply(ops, detail="brief")
    for i in range(4):
        for part in ("n", "t", "c"):
            move_ink(p, f"s-{part}{i}", cy=top + i * 104 + 43)
    kit.save(p, "social", "story-1080x1920", [("png", {})], thumbnail_width=540,
             safe_area={"left": 60, "right": 60, "top": 250, "bottom": 250})


def build_joint_squares(kit):
    b = kit.b
    W = H = 1080
    M = 88
    # 1 statement
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(-300, -300, 1300), strength="50"), detail="brief")
    p.apply([text("kicker", JOINT["kicker"], M, 120, 22, b["accent"], MONO),
             headline(b, "head", JOINT["hero_tall"], M, 200, 120, JOINT["tall_accent"], line_height=0.96)], detail="brief")
    p.apply(link_line("link", M, bottom(p, "head") + 50, 280, 8), detail="brief")
    joint_lockup(p, "logo", M, H - 150, 46)
    kit.save(p, "social", "post-1-statement-1080", [("png", {})], thumbnail_width=540)
    # 2 who does what
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(300, -500, 1300)), detail="brief")
    p.apply([text("kicker", "WHO DOES WHAT", M, 110, 22, b["accent"], MONO),
             headline(b, "head", "Two jobs.\nTwo programs.", M, 160, 90, "Two programs.", accent_color=b["teal"])],
            detail="brief")
    top = bottom(p, "head") + 70
    cw = (W - 2 * M - 30) / 2
    for side, (brand, jobs, label) in enumerate([(JX, JOINT["jx_jobs"], "JX Runtime"),
                                                (ONYX, JOINT["onyx_jobs"], "Onyx Engine")]):
        cx = round(M + side * (cw + 30))
        ops = [rect(f"c{side}", cx, top, cw, 560, b["panel"], radius=26, stroke=brand["accent"], stroke_width=3),
               text(f"c{side}-h", label, cx + 40, top + 40, 40, brand["text"], brand["head"])]
        for i, job in enumerate(jobs):
            ops += [rect(f"c{side}-px{i}", cx + 40, top + 136 + i * 66, 16, 16, brand["accent"]),
                    text(f"c{side}-j{i}", job, cx + 72, top + 126 + i * 66, 27, b["muted"], b["body"])]
        p.apply(ops, detail="brief")
    p.apply(onyx_mark("gem", M + round(cw) + 30 + round(cw) - 90, top + 30, 56), detail="brief")
    place_jx_mark(p, "tile", M + round(cw) - 90, top + 30, 56)
    kit.save(p, "social", "post-2-who-does-what-1080", [("png", {})], thumbnail_width=540)
    # 3 get started
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(100, 400, 1300), strength="45"), detail="brief")
    p.apply([text("kicker", "SWITCH ENGINES", M, 110, 22, b["accent"], MONO),
             headline(b, "head", "Put Onyx behind\nJX Runtime.", M, 160, 90, "JX Runtime.")], detail="brief")
    lines = [(0, [("$ ", JX["accent"]), ("jx-runtime backend install --engine onyx", b["text"])]),
             (0, [("# downloads onyx-engine, checks it against", CODE_DIM)]),
             (0, [("# the release's checksums.txt", CODE_DIM)]),
             (0, [("", b["text"])]),
             (0, [("// config", CODE_DIM)]),
             (0, [("\"execution\"", CODE_KEY), (": { ", b["text"]), ("\"backend\"", CODE_KEY), (": ", b["text"]),
                  ("\"onyxengine\"", ONYX["accent2"]), (" }", b["text"])]),
             (0, [("", b["text"])]),
             (0, [("$ ", JX["accent"]), ("jx-runtime serve", b["text"])])]
    p.apply(code_card(b, "code", M, bottom(p, "head") + 64, W - 2 * M, lines, size=26), detail="brief")
    fit_code(p, "code")
    joint_lockup(p, "logo", M, H - 130, 40)
    kit.save(p, "social", "post-3-switch-engines-1080", [("png", {})], thumbnail_width=540)


def build_joint_diagram(kit):
    """Architecture diagram, 1920 x 1080: PNG, SVG and PDF."""
    b = kit.b
    W, H, M = 1920, 1080, 120
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(400, -900, 1800), strength="30"), detail="brief")
    p.apply([text("kicker", "HOW THEY FIT", M, 90, 24, b["accent"], MONO),
             headline(b, "head", "One endpoint in front. One engine per model behind.", M, 136, 58,
                      "One engine per model behind.", accent_color=b["teal"])], detail="brief")
    architecture(p, b, "arch", M, 330, W - 2 * M, 560, size=1.0)
    p.apply(para("note", "Engines bind to 127.0.0.1 and are reached only through JX Runtime. Keys, pairing and the "
                 "audit log belong to the runtime; batching, grammars and streaming belong to the engine.",
                 M, 950, 24, W - 2 * M, b["muted"], b["body"]), detail="brief")
    kit.save(p, "diagram", "architecture-1920x1080", [("png", {}), ("svg", {}), ("pdf", {})],
             checks=["bounds", "contrast", "fonts"])


def build_joint_carousel(kit):
    b = kit.b
    W, H, M = 1080, 1350, 88
    p = kit.new(W, H)
    p.apply([{"type": "master", "action": "add", "name": "frame", "background": b["bg"]},
             *scanlines("m-lines", 0, 12, W, H - 12, 14, "#FFFFFF07"),
             text("m-num", "${page} / ${pages}", W - M - 80, H - 100, 24, b["muted"], MONO)], detail="brief")
    joint_lockup(p, "m-logo", M, H - 112, 34)

    def page(name):
        p.apply({"type": "page", "action": "add", "name": name, "master": "frame"}, detail="brief")

    page("cover")
    p.apply([glow("c-glow", -300, -300, 1400, JX["accent"], "45"), glow("c-glow2", 300, 700, 1200, ONYX["accent"], "30"),
             *decoration("c-glow", "c-glow2"), text("c-kicker", JOINT["kicker"], M, 170, 22, b["accent"], MONO)],
            detail="brief")
    place_jx_mark(p, "c-tile", M, 250, 150)
    p.apply(onyx_mark("c-gem", M + 190, 250, 150), detail="brief")
    p.apply(headline(b, "c-title", JOINT["hero_tall"], M, 470, 124, JOINT["tall_accent"], line_height=0.96), detail="brief")
    p.apply(link_line("c-link", M, bottom(p, "c-title") + 50, 300, 8), detail="brief")

    for i, (step, cmd, desc) in enumerate(JOINT["steps"]):
        page(f"step-{i + 1}")
        color = JX["accent"] if i in (0, 3) else ONYX["accent"]
        p.apply([text(f"s{i}-kicker", f"STEP {i + 1} OF 4", M, 150, 24, color, MONO),
                 text(f"s{i}-num", f"0{i + 1}", M, 230, 220, color, b["head"]),
                 headline(b, f"s{i}-title", step + ".", M, 520, 140, step + ".", accent_color=color)],
                detail="brief")
        p.apply(code_card(b, f"s{i}-code", M, bottom(p, f"s{i}-title") + 70, W - 2 * M,
                          [(0, [(cmd, b["text"])])], size=26, chrome=False), detail="brief")
        fit_code(p, f"s{i}-code")
        p.apply(para(f"s{i}-d", desc, M, bottom(p, f"s{i}-code-bg") + 50, 36, W - 2 * M, b["muted"], b["body"]),
                detail="brief")

    page("split")
    p.apply([text("w-kicker", "WHO DOES WHAT", M, 150, 24, b["accent"], MONO),
             headline(b, "w-title", "Execution and\naccess, kept\napart.", M, 220, 90, "kept\napart.",
                      accent_color=b["teal"])], detail="brief")
    top = bottom(p, "w-title") + 70
    cw = (W - 2 * M - 30) / 2
    ops = []
    for side, (brand, jobs, label) in enumerate([(JX, JOINT["jx_jobs"], "JX Runtime"),
                                                (ONYX, JOINT["onyx_jobs"], "Onyx Engine")]):
        cx = round(M + side * (cw + 30))
        ops += [rect(f"w{side}", cx, top, cw, 520, b["panel"], radius=26, stroke=brand["accent"], stroke_width=3),
                text(f"w{side}-h", label, cx + 36, top + 36, 38, brand["text"], brand["head"])]
        for i, job in enumerate(jobs):
            ops += [rect(f"w{side}-px{i}", cx + 36, top + 128 + i * 62, 14, 14, brand["accent"]),
                    text(f"w{side}-j{i}", job, cx + 66, top + 118 + i * 62, 25, b["muted"], b["body"])]
    p.apply(ops, detail="brief")

    names = ["cover", "step-1", "step-2", "step-3", "step-4", "split"]
    kit.save(p, "carousel", "carousel-1080x1350", [("pdf", {})], thumbnail_width=540,
             checks=["bounds", "overlap", "contrast", "fonts", "deck"], deck={"profile": "phone"})
    for i, name in enumerate(names, 1):
        p.export(str(kit.out / "carousel" / f"slide-{i}-{name}.png"), page=name, overwrite=True)
    kit.sheet(p, "carousel", "contact-sheet.png", width=360, columns=6)


def build_joint_deck(kit):
    b = kit.b
    p, W, H, M = deck_base(kit)

    deck_slide(p, "title", "JX Runtime and Onyx Engine: a runtime and the engine it was built to run.", master=None)
    p.apply(backdrop(b, W, H, glow_at=(600, -600, 1900), strength="45"), detail="brief")
    joint_lockup(p, "t-logo", M, 150, 70)
    p.apply(headline(b, "t-title", JOINT["hero_tall"], M, 330, 104, JOINT["tall_accent"], line_height=0.98), detail="brief")
    p.apply(link_line("t-link", M, bottom(p, "t-title") + 60, 360, 8), detail="brief")
    place_jx_mark(p, "t-tile", W - 640, 300, 260)
    p.apply(onyx_mark("t-gem", W - 420, 470, 300), detail="brief")

    deck_slide(p, "two", "Each project does one job. The split is the point.")
    y = deck_title(p, b, "tw", "TWO PROJECTS", "Execution and access, kept apart.", "kept apart.")
    cw = (W - 2 * M - 40) / 2
    ops = []
    for side, (brand, jobs, label, line) in enumerate([
            (JX, JOINT["jx_jobs"], "JX Runtime", "The local model runtime you can expose to someone other than yourself."),
            (ONYX, JOINT["onyx_jobs"], "Onyx Engine", "A single-model, OpenAI-compatible server built on llama.cpp.")]):
        cx = round(M + side * (cw + 40))
        ops += [rect(f"tw{side}", cx, y + 60, cw, 600, b["panel"], radius=28, stroke=brand["accent"], stroke_width=3),
                text(f"tw{side}-h", label, cx + 150, y + 104, 50, brand["text"], brand["head"]),
                para(f"tw{side}-l", line, cx + 50, y + 200, 28, round(cw - 100), brand["accent2"], brand["body"])]
        for i, job in enumerate(jobs):
            ops += [rect(f"tw{side}-px{i}", cx + 50, y + 318 + i * 54, 16, 16, brand["accent"]),
                    text(f"tw{side}-j{i}", job, cx + 86, y + 306 + i * 54, 30, b["muted"], b["body"])]
    p.apply(ops, detail="brief")
    place_jx_mark(p, "tw-tile", M + 50, y + 94, 80)
    p.apply(onyx_mark("tw-gem", round(M + cw + 40 + 50), y + 94, 80), detail="brief")

    deck_slide(p, "arch", "Clients reach one endpoint; JX Runtime runs one onyx-engine per loaded model.")
    y = deck_title(p, b, "ar", "HOW THEY FIT", "One endpoint. One engine per model.", "One engine per model.", size=80)
    architecture(p, b, "ar-d", M, y + 110, W - 2 * M, 520, size=0.95)

    deck_slide(p, "steps", "From a stock JX Runtime to Onyx Engine in four steps.")
    y = deck_title(p, b, "st", "SWITCH ENGINES", "Four steps. One setting.", "One setting.")
    cw = (W - 2 * M - 3 * 30) / 4
    ops = []
    for i, (step, cmd, desc) in enumerate(JOINT["steps"]):
        cx = round(M + i * (cw + 30))
        color = JX["accent"] if i in (0, 3) else ONYX["accent"]
        ops += [rect(f"st-c{i}", cx, y + 70, cw, 560, b["panel"], radius=24, stroke=b["line"], stroke_width=2),
                rect(f"st-bar{i}", cx, y + 70, cw, 8, color),
                text(f"st-n{i}", f"0{i + 1}", cx + 36, y + 120, 34, color, b["head"]),
                text(f"st-t{i}", step, cx + 36, y + 180, 50, b["text"], b["head"]),
                para(f"st-cmd{i}", cmd, cx + 36, y + 266, 21, round(cw - 72), CODE_STR, MONO, line_height=1.3),
                para(f"st-d{i}", desc, cx + 36, y + 390, 25, round(cw - 72), b["muted"], b["body"])]
    p.apply(ops, detail="brief")

    deck_slide(p, "compare", "jx-runtime dual-engine runs two instances on the same model files.")
    y = deck_title(p, b, "cm", "SIDE BY SIDE", "Compare engines on the same models.", "the same models.", size=80)
    lines = [(0, [("$ ", JX["accent"]), ("jx-runtime dual-engine plan", b["text"]), ("      # prints, changes nothing", CODE_DIM)]),
             (0, [("$ ", JX["accent"]), ("jx-runtime dual-engine setup --yes", b["text"])]),
             (0, [("", b["text"])]),
             (0, [("$ ", JX["accent"]), ("jx-runtime serve", b["text"]),
                  ("                                 # :8712  llama.cpp", CODE_DIM)]),
             (0, [("$ ", JX["accent"]), ("jx-runtime serve --data-dir ~/.jx-runtime-onyx", b["text"]),
                  ("   # :8713  Onyx Engine", ONYX["accent2"])])]
    p.apply(code_card(b, "cm-code", M, y + 80, W - 2 * M, lines, size=30), detail="brief")
    fit_code(p, "cm-code")
    p.apply(para("cm-note", "The secondary borrows the primary's model folders. Keys, pairing and the audit log are "
                 "per instance and never copied.", M, bottom(p, "cm-code-bg") + 50, 30, W - 2 * M, b["muted"],
                 b["body"]), detail="brief")

    deck_slide(p, "same-code", "The onyxengine adapter subclasses the llama.cpp adapter.")
    y = deck_title(p, b, "sc", "NO SECOND COPY", "Same contract. Same code path.", "Same code path.")
    pts = [("/health", "Polled until ready. For onyx-engine, ready means the model is loaded."),
           ("/props", "Context, template hash and build info, read once after start."),
           ("/v1/*", "Chat, completions and embeddings proxied straight through, streaming included."),
           ("restart", "Crash detection and failure classification are the llama.cpp adapter's own code.")]
    ops = []
    for i, (k, d) in enumerate(pts):
        yy = y + 110 + i * 130
        ops += [rect(f"sc-c{i}", M, yy, W - 2 * M, 106, b["panel"], radius=18, stroke=b["line"], stroke_width=2),
                text(f"sc-k{i}", k, M + 40, yy + 34, 32, ONYX["accent"], MONO),
                text(f"sc-d{i}", d, M + 340, yy + 34, 30, b["text"], b["body"])]
    p.apply(ops, detail="brief")

    deck_slide(p, "start", "Links and status.", master=None)
    p.apply(backdrop(b, W, H, glow_at=(-300, 200, 1800), strength="50"), detail="brief")
    p.apply(headline(b, "end-title", "Run it on your machine.\nShare it on your terms.", M, 200, 100,
                     "Share it on your terms."), detail="brief")
    p.apply(link_line("end-link", M, bottom(p, "end-title") + 60, 360, 8), detail="brief")
    top = bottom(p, "end-title") + 130
    for i, brand in enumerate((JX, ONYX)):
        lockup(p, brand, f"end-l{i}", M + i * 820, top, 70)
        p.apply([text(f"end-u{i}", brand["repo"], M + i * 820, top + 110, 26, brand["accent2"], MONO),
                 para(f"end-s{i}", COPY[brand["key"]]["status"], M + i * 820, top + 160, 24, 720, b["muted"],
                      b["body"])], detail="brief")
    kit.save(p, "deck", "jx-runtime-onyx-engine-deck", [("pdf", {}), ("pptx", {}), ("html", {})],
             checks=["bounds", "overlap", "contrast", "fonts", "deck"], deck={"profile": "screen"})
    kit.sheet(p, "deck", "contact-sheet.png", width=480, columns=4)
    p.export(str(kit.out / "deck" / "deck-title.png"), page="title", scale=0.5, overwrite=True)


def build_joint_onepager(kit):
    b = kit.b
    p = kit.new(100, 100, "#FFFFFF")
    p.apply({"type": "canvas", "size": "letter", "dpi": 150, "background": "#FFFFFF"}, detail="brief")
    W, H = p.state["canvas"]["width"], p.state["canvas"]["height"]
    M = 90
    p.apply([rect("band", 0, 0, W, 520, b["bg"]), glow("glow", 500, -520, 1200, JX["accent"], "45"),
             glow("glow2", -300, 100, 900, ONYX["accent"], "25"), *decoration("glow", "glow2")], detail="brief")
    joint_lockup(p, "logo", M, 70, 44)
    p.apply(headline(b, "head", JOINT["hero"], M, 190, 62, JOINT["accent"], line_height=1.04), detail="brief")
    p.apply(para("sub", JOINT["sub"], M, bottom(p, "head") + 28, 22, W - 2 * M - 40, b["muted"], b["body"]),
            detail="brief")
    p.apply({"type": "resize", "target": "band", "height": round(bottom(p, "sub") + 56)}, detail="brief")
    p.apply(link_line("link", 0, bottom(p, "band"), W, 8), detail="brief")
    top = bottom(p, "band") + 50
    cw = (W - 2 * M - 40) / 2
    ops = []
    for side, (brand, jobs, label) in enumerate([(JX, JOINT["jx_jobs"], "JX Runtime does"),
                                                (ONYX, JOINT["onyx_jobs"], "Onyx Engine does")]):
        cx = round(M + side * (cw + 40))
        ops += [rect(f"j{side}-bar", cx, top, cw, 6, brand["accent"]),
                text(f"j{side}-h", label, cx, top + 26, 28, b["ink"], brand["head"])]
        for i, job in enumerate(jobs):
            ops += [rect(f"j{side}-px{i}", cx, top + 96 + i * 46, 13, 13, brand["accent"] if side == 0 else brand["accent_deep"]),
                    text(f"j{side}-t{i}", job, cx + 28, top + 86 + i * 46, 24, b["subtle"], b["body"])]
    p.apply(ops, detail="brief")
    top = top + 96 + 6 * 46 + 30
    p.apply(text("sh", "Switch JX Runtime to Onyx Engine", M, top, 26, b["ink"], b["head"]), detail="brief")
    y = bottom(p, "sh") + 24
    ops = []
    for i, (step, cmd, desc) in enumerate(JOINT["steps"]):
        yy = y + i * 118
        color = JX["accent"] if i in (0, 3) else ONYX["accent_deep"]
        ops += [text(f"s-n{i}", f"0{i + 1}", M, yy, 30, color, b["head"]),
                text(f"s-t{i}", step, M + 66, yy + 2, 26, b["ink"], b["head"]),
                text(f"s-c{i}", cmd, M + 220, yy + 6, 20, "#3A3842", MONO),
                para(f"s-d{i}", desc, M + 220, yy + 42, 20, W - 2 * M - 220, b["subtle"], b["body"])]
    p.apply(ops, detail="brief")
    p.apply([rect("foot-rule", M, H - 130, W - 2 * M, 2, "#DCDAE0"),
             text("foot-a", JX["repo"], M, H - 100, 17, b["ink"], MONO),
             text("foot-b", ONYX["repo"], 0, H - 100, 17, b["ink"], MONO),
             text("foot-s", "Both MIT licensed. JX Runtime is alpha; its release binaries are not yet code-signed.",
                  M, H - 64, 15, b["subtle"], b["body"])], detail="brief")
    move_ink(p, "foot-b", rx=W - M)
    kit.save(p, "print", "one-pager-letter", [("pdf", {}), ("png", {"scale": 0.6})],
             checks=["bounds", "overlap", "contrast", "fonts"])


def build_joint_poster(kit):
    """Tabloid (11 x 17 in) poster with bleed: CMYK PDF and PNG preview."""
    b = kit.b
    p = kit.new(100, 100)
    p.apply({"type": "canvas", "size": "tabloid", "dpi": 150, "bleed": True, "background": b["bg"]}, detail="brief")
    W, H = p.state["canvas"]["width"], p.state["canvas"]["height"]
    M = 150
    p.apply([glow("g1", -600, -400, 2400, JX["accent"], "50"), glow("g2", 300, 1400, 2200, ONYX["accent"], "35"),
             *decoration("g1", "g2"), *scanlines("lines", 0, 20, W, H - 20, 18, "#FFFFFF06"),
             text("kicker", JOINT["kicker"], M, 180, 32, b["accent"], MONO)], detail="brief")
    place_jx_mark(p, "tile", M, 300, 360)
    p.apply([text("plus", "+", M + 420, 380, 180, b["muted"], b["semi"]), *onyx_mark("gem", M + 600, 300, 380)],
            detail="brief")
    p.apply(headline(b, "head", "ONYX RUNS\nTHE MODEL.\nJX GUARDS\nTHE DOOR.", M, 800, 196, "JX GUARDS\nTHE DOOR.",
                     line_height=0.9), detail="brief")
    p.apply(link_line("link", M, bottom(p, "head") + 70, 700, 14), detail="brief")
    top = bottom(p, "head") + 150
    p.apply(para("sub", JOINT["sub"], M, top, 40, W - 2 * M, b["muted"], b["body"], line_height=1.35), detail="brief")
    top = bottom(p, "sub") + 80
    cw = (W - 2 * M - 60) / 2
    ops = []
    for side, (brand, jobs, label) in enumerate([(JX, JOINT["jx_jobs"], "JX Runtime"), (ONYX, JOINT["onyx_jobs"], "Onyx Engine")]):
        cx = round(M + side * (cw + 60))
        ops += [rect(f"c{side}-bar", cx, top, cw, 8, brand["accent"]),
                text(f"c{side}-h", label, cx, top + 40, 60, brand["text"], brand["head"])]
        for i, job in enumerate(jobs[:3]):
            ops.append(text(f"c{side}-j{i}", job, cx, top + 130 + i * 56, 34, b["muted"], b["body"]))
    p.apply(ops, detail="brief")
    top = bottom(p, "c0-j2") + 80
    p.apply([text("u1", JX["repo"], M, top, 32, JX["accent2"], MONO),
             text("u2", ONYX["repo"], M, top + 52, 32, ONYX["accent2"], MONO)], detail="brief")
    kit.save(p, "print", "poster-tabloid", [("png", {"scale": 0.5}), ("pdf", {"color_space": "cmyk"})],
             checks=["bounds", "overlap", "contrast", "safe_area", "fonts"])


def build_joint_stickers(kit):
    """US Letter sheet of six die-cut style stickers."""
    p = kit.new(100, 100, "#FFFFFF")
    p.apply({"type": "canvas", "size": "letter", "dpi": 150, "background": "#FFFFFF"}, detail="brief")
    H = p.state["canvas"]["height"]
    D = 480
    cells = [(100, 70), (695, 70), (100, 575), (695, 575), (100, 1080), (695, 1080)]
    fills = [JX["bg"], ONYX["bg"], "#FFFFFF", JX["accent"], JX["bg"], ONYX["panel"]]
    p.apply([{"type": "shape", "shape": "rounded-rectangle" if i in (1, 4) else "ellipse", "name": f"cut{i}",
              "x": x, "y": y, "width": D, "height": D, "radius": 90, "fill": fills[i], "stroke": "#C9CCD6",
              "stroke_width": 3} for i, (x, y) in enumerate(cells)], detail="brief")
    (x0, y0), (x1, y1), (x2, y2), (x3, y3), (x4, y4), (x5, y5) = cells
    place_jx_mark(p, "s0", x0 + 110, y0 + 110, D - 220)
    p.apply(onyx_mark("s1", x1 + 90, y1 + 90, D - 180), detail="brief")
    p.apply([text("s2-a", "POWERED BY", 0, y2 + 130, 34, JX["ink"], MONO)], detail="brief")
    lockup(p, ONYX, "s2-l", x2 + 40, y2 + 210, 70, light=True)
    move_ink(p, "s2-l", cx=x2 + D / 2)
    p.apply([headline(JX, "s3-t", "KEYS\nON BY\nDEFAULT", x3, y3 + 100, 82, align="center", width=D, color="#FFFFFF",
                      line_height=0.95),
             headline(JX, "s4-t", "The runtime\nexecutes.\nThe app\ndecides.", x4, y4 + 70, 62, "decides.",
                      align="center", width=D, line_height=1.0),
             headline(ONYX, "s5-t", "one\nmodel,\none\nprocess.", x5, y5 + 60, 78, "process.", align="center", width=D,
                      line_height=0.92)], detail="brief")
    move_ink(p, "s2-a", cx=x2 + D / 2)
    for n, (cx, cy) in (("s3-t", (x3, y3)), ("s4-t", (x4, y4)), ("s5-t", (x5, y5))):
        move_ink(p, n, cy=cy + D / 2)
    p.apply(text("note", "Cut along the grey outlines. Kiss-cut on matte vinyl.", 100, H - 70, 22, "#55525C",
                 JX["body"]), detail="brief")
    kit.save(p, "print", "sticker-sheet-letter", [("pdf", {}), ("png", {"scale": 0.5})],
             checks=["bounds", "overlap", "contrast", "fonts"])


def build_joint_teaser(kit):
    b = kit.b
    W = H = 1080
    p = kit.new(W, H)
    p.apply(backdrop(b, W, H, glow_at=(-200, -200, 1500), strength="45"), detail="brief")
    place_jx_mark(p, "mark-jx", 250, 250, 260)
    p.apply(onyx_mark("mark-onyx", 570, 250, 260), detail="brief")
    p.apply({"type": "group", "name": "mark", "targets": ["mark-jx", "mark-onyx"]}, detail="brief")
    words = JOINT["words"]
    p.apply([headline(b, f"w{i}", w, 0, 650, 120, w, align="center", width=W,
                      accent_color=JX["accent"] if i in (0, 3) else ONYX["accent"]) for i, w in enumerate(words)],
            detail="brief")
    joint_lockup(p, "end-logo", 0, 400, 56)
    move_ink(p, "end-logo", cx=W / 2)
    p.apply([text("end-line", "Onyx runs the model. JX guards the door.", 0, 560, 40, b["text"], b["semi"]),
             link_line("end-link", 390, 650, 300, 6),
             text("end-url", "jx-runtime backend install --engine onyx", 0, 700, 28, b["muted"], MONO),
             {"type": "timeline-set", "duration": 6600, "fps": 24}], detail="brief")
    move_ink(p, "end-line", cx=W / 2)
    move_ink(p, "end-url", cx=W / 2)
    p.apply({"type": "group", "name": "end-url-g", "targets": ["end-link"]}, detail="brief")
    teaser_motion(p, len(words))
    p.apply([{"type": "keyframe", "target": "end-url-g", "property": "opacity", "time": 0, "value": 0, "easing": "hold"},
             {"type": "keyframe", "target": "end-url-g", "property": "opacity", "time": 4500, "value": 0},
             {"type": "keyframe", "target": "end-url-g", "property": "opacity", "time": 4800, "value": 1}],
            detail="brief")
    write_motion(kit, p, "teaser")


def build_overview(kit):
    """One board showing every piece of this kit, framed from the exports above."""
    b = kit.b
    W, H = 2400, 1600
    p = kit.new(W, H)
    o = str(kit.out)
    joint = b["key"] == "together"
    tiles = [
        (f"{o}/social/og-card-1200x630.png", 80, 230, 900, 473, "Open Graph card"),
        (f"{o}/social/github-preview-1280x640.png", 80, 770, 900, 450, "GitHub social preview"),
        (f"{o}/carousel/slide-1-cover.png", 1030, 230, 400, 500, "Carousel"),
        (f"{o}/print/one-pager-letter.png", 1030, 790, 400, 518, "Product sheet"),
        (f"{o}/social/story-1080x1920.png", 1480, 230, 360, 640, "Story"),
        (f"{o}/social/post-1-statement-1080.png", 1480, 930, 360, 360, "Feed posts"),
        (f"{o}/deck/deck-title.png", 1890, 230, 430, 242, "Pitch deck"),
        (f"{o}/motion/teaser-frame.png", 1890, 540, 300, 300, "Motion teaser"),
        (f"{o}/social/linkedin-banner-1584x396.png", 1890, 910, 430, 108, "LinkedIn banner"),
        (f"{o}/social/x-header-1500x500.png", 1890, 1100, 430, 143, "X header"),
        (f"{o}/print/poster-tabloid.png" if joint else f"{o}/logos/lockup-dark.png", 80, 1290, 900 if not joint else 0,
         230, "Logos"),
    ]
    if joint:
        tiles[-1] = (f"{o}/diagram/architecture-1920x1080.png", 80, 1290, 410, 231, "Architecture diagram")
        tiles.append((f"{o}/print/poster-tabloid.png", 540, 1290, 150, 232, "Poster"))
        tiles.append((f"{o}/print/sticker-sheet-letter.png", 740, 1290, 180, 233, "Stickers"))
    p.apply(backdrop(b, W, H, glow_at=(600, -1200, 2400), strength="30"), detail="brief")
    if joint:
        joint_lockup(p, "logo", 80, 80, 60)
    else:
        lockup(p, b, "logo", 80, 80, 70)
    p.apply(text("head", "Marketing kit, made with Vixl 0.23", 0, 92, 48, b["text"], b["head"]), detail="brief")
    move_ink(p, "head", rx=W - 80, cy=118 if not joint else 110)
    ops = []
    for i, (path, x, y, w, h, label) in enumerate(tiles):
        ops += [rect(f"t{i}-base", x, y, w, h, b["panel"], radius=12),
                {"type": "frame", "name": f"t{i}", "path": path, "x": x, "y": y, "width": w, "height": h, "fit": "fit"},
                {"type": "clip", "target": f"t{i}", "base": f"t{i}-base"}]
    p.apply(ops, detail="brief")
    for i, (path, x, y, w, h, label) in enumerate(tiles):
        chip(p, b, f"lab{i}", label, x + 10, y + h + 10 if h < 160 or y > 1200 else y + h - 46, 18,
             b["text"], "#000000D0", pad=(12, 8))
    kit.save(p, ".", "kit-overview", [("png", {"scale": 0.75})], checks=["bounds", "fonts"])


SOLO = {"logos": build_logos, "og": build_og, "github": build_github, "linkedin": build_linkedin,
        "xheader": build_xheader, "story": build_story, "squares": build_squares, "carousel": build_carousel,
        "deck": build_deck, "onepager": build_onepager, "teaser": build_teaser, "overview": build_overview}
JOINT_PIECES = {"og": build_joint_og, "github": build_joint_github, "linkedin": build_joint_linkedin,
                "xheader": build_joint_xheader, "story": build_joint_story, "squares": build_joint_squares,
                "diagram": build_joint_diagram, "carousel": build_joint_carousel, "deck": build_joint_deck,
                "onepager": build_joint_onepager, "poster": build_joint_poster, "stickers": build_joint_stickers,
                "teaser": build_joint_teaser, "overview": build_overview}
KITS = {"jx": (JX, SOLO), "onyx": (ONYX, SOLO), "together": (TOGETHER, JOINT_PIECES)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--kit", choices=list(KITS), action="append",
                        help=f"kit to build (repeatable); default: {REPO_KIT} and together")
    parser.add_argument("pieces", nargs="*", help="pieces to build (default: all)")
    args = parser.parse_args()
    for key in args.kit or [REPO_KIT, "together"]:
        brand, pieces = KITS[key]
        kit = Kit(brand)
        for name in args.pieces or list(pieces):
            if name not in pieces:
                continue
            print(f"[{key}:{name}]")
            pieces[name](kit)


if __name__ == "__main__":
    main()
