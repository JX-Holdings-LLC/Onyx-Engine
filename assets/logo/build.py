"""Builds this repository's logo pack with the vixl CLI: python build.py [OUT_DIR]."""
import json, math, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else HERE
WORK = tempfile.mkdtemp(prefix="logo-pack-")
ICON_SIZES = [16, 32, 48, 64, 128, 180, 192, 256, 512, 1024]
LOGO_WIDTHS = [400, 800, 1600]


def vixl(*args):
    r = subprocess.run(["vixl", "--json", *args], capture_output=True, text=True, cwd=WORK)
    if r.returncode:
        sys.exit(f"vixl {' '.join(args)} failed:\n{r.stderr}\n{r.stdout}")
    return r.stdout


def document(name, ops, fonts, size, bg="transparent", fit=None, pad=0):
    """Build NAME.vixl from ops; with fit=LAYER, shrink the canvas to that layer plus pad."""
    doc = os.path.join(WORK, f"{name}.vixl")
    vixl("new", size, "-o", doc, "--background", bg)
    for family, weight in fonts:
        vixl("-p", doc, "font", "install", family, "--weight", str(weight))
    ops_file = os.path.join(WORK, f"{name}.json")
    with open(ops_file, "w") as fh:
        json.dump(ops, fh)
    vixl("-p", doc, "apply", ops_file)
    if fit:
        x, y, w, h = json.loads(vixl("-p", doc, "inspect", fit))["resolved_bounds"]
        with open(ops_file, "w") as fh:
            json.dump([{"type": "canvas", "width": w + 2 * pad, "height": h + 2 * pad},
                       {"type": "align", "target": fit, "alignment": "center"}], fh)
        vixl("-p", doc, "apply", ops_file)
        return doc, w + 2 * pad, h + 2 * pad
    w, h = (int(v) for v in size.split("x"))
    return doc, w, h


def export(doc, path, *extra):
    path = os.path.join(OUT, path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    vixl("-p", doc, "export", path, "--overwrite", *extra)


def logo_set(doc, stem, width):
    """SVG, PDF and PNGs at LOGO_WIDTHS for a lockup whose canvas is WIDTH px wide."""
    export(doc, f"svg/{stem}.svg", "--svg-policy", "strict")
    export(doc, f"pdf/{stem}.pdf")
    for w in LOGO_WIDTHS:
        export(doc, f"png/{stem}-{w}w.png", "--scale", f"{w / width:.6f}")


def icon_set(doc, stem, size=1024):
    export(doc, f"svg/{stem}.svg", "--svg-policy", "strict")
    for s in ICON_SIZES:
        export(doc, f"png/{stem}-{s}.png", "--scale", f"{s / size:.6f}")


def circle(cx, cy, r, cw=True):
    s = 1 if cw else 0
    return f"M {cx - r} {cy} A {r} {r} 0 0 {s} {cx + r} {cy} A {r} {r} 0 0 {s} {cx - r} {cy} Z"


def poly(pts):
    return "M " + " L ".join(f"{x:.2f} {y:.2f}" for x, y in pts) + " Z"


def hexagon(cx, cy, r, rot=0):
    return poly([(cx + r * math.cos(math.radians(rot + 60 * i)), cy + r * math.sin(math.radians(rot + 60 * i)))
                 for i in range(6)])


def path(name, d, fill, w, h, x=0, y=0, **kw):
    return {"type": "shape", "shape": "path", "name": name, "path": d, "fill": fill,
            "x": x, "y": y, "width": w, "height": h, **kw}


def finish():
    shutil.rmtree(WORK, ignore_errors=True)
    print("wrote", OUT)


ONYX, CYAN, CYAN_DEEP, STEEL, GRAPHITE, BODY, WHITE = (
    "#0E0F12", "#4FD1FF", "#0A84B8", "#6B7385", "#2A2E37", "#454A57", "#F2F4F8")
FONTS = [("Space Grotesk", 700), ("Space Grotesk", 500)]


def nut(s=1.0, body=BODY, face=GRAPHITE, edge=STEEL, bore=CYAN, mono=None, name="mark"):
    """Hex nut seen from above: hex body, chamfered face, lit bore. mono=COLOR draws it in one colour."""
    c, W = 120 * s, 240 * s
    if mono:
        ops = [path("body", hexagon(c, c, 120 * s) + " " + circle(c, c, 66 * s, cw=False), mono, W, W),
               path("ring", circle(c, c, 56 * s) + " " + circle(c, c, 44 * s, cw=False), mono, W, W)]
        return ops + [{"type": "group", "name": name, "targets": ["body", "ring"]}]
    return [
        path("body", hexagon(c, c, 120 * s) + " " + circle(c, c, 50 * s, cw=False), body, W, W),
        path("face", circle(c, c, 100 * s) + " " + circle(c, c, 50 * s, cw=False), face, W, W,
             stroke=edge, stroke_width=3 * s),
        path("bore", circle(c, c, 50 * s), "none", W, W, stroke=bore, stroke_width=7 * s),
        {"type": "group", "name": name, "targets": ["body", "face", "bore"]},
    ]


def wordmark(word, sub):
    return [
        {"type": "text", "text": "ONYX", "name": "word", "font": "space-grotesk-700", "size": 120, "color": word},
        {"type": "text", "text": "E N G I N E", "name": "sub", "font": "space-grotesk-500", "size": 34, "color": sub},
        {"type": "constrain", "target": "sub", "constraints": {"center-x": "word.center-x", "top": "word.bottom+10"}},
        {"type": "group", "name": "type", "targets": ["word", "sub"]},
    ]


def lockup(mark_ops, word, sub):
    return [
        *mark_ops, *wordmark(word, sub),
        {"type": "constrain", "target": "type", "constraints": {"left": "mark.right+56", "center-y": "mark.center-y"}},
        {"type": "group", "name": "lockup", "targets": ["mark", "type"]},
        {"type": "align", "target": "lockup", "alignment": "center"},
    ]


VARIANTS = {
    "logo-on-dark": lockup(nut(), WHITE, CYAN),
    "logo-on-light": lockup(nut(bore=CYAN_DEEP), ONYX, CYAN_DEEP),
    "logo-mono-black": lockup(nut(mono="#000000"), "#000000", "#000000"),
    "logo-mono-white": lockup(nut(mono="#FFFFFF"), "#FFFFFF", "#FFFFFF"),
}


def build():
    for stem, ops in VARIANTS.items():
        doc, w, _ = document(stem, ops, FONTS, "1600x800", fit="lockup", pad=24)
        logo_set(doc, stem, w)

    doc, _, _ = document("mark", nut(s=1024 / 240), FONTS, "1024x1024")
    icon_set(doc, "mark")

    icon = [
        {"type": "shape", "shape": "rounded-rectangle", "name": "tile", "width": 1024, "height": 1024,
         "radius": 224, "fill": ONYX, "x": 0, "y": 0},
        *nut(s=3.15),
        {"type": "align", "target": "mark", "alignment": "center", "relative_to": "tile"},
    ]
    doc, _, _ = document("icon", icon, FONTS, "1024x1024")
    icon_set(doc, "icon")
    export(doc, "favicon.ico", "--icon-sizes", "16", "32", "48")

    social = [
        {"type": "solid", "name": "bg", "color": ONYX},
        *lockup(nut(), WHITE, CYAN),
        {"type": "text", "text": "Single-model, OpenAI-compatible serving on llama.cpp", "name": "tagline",
         "font": "space-grotesk-500", "size": 32, "color": "#8A93A6"},
        {"type": "constrain", "target": "tagline", "constraints": {"center-x": "lockup.center-x", "top": "lockup.bottom+56"}},
        {"type": "group", "name": "content", "targets": ["lockup", "tagline"]},
        {"type": "align", "target": "content", "alignment": "center"},
    ]
    doc, _, _ = document("social-preview", social, FONTS, "1280x640")
    export(doc, "social-preview.png")


build()
finish()
