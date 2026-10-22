"""Drawing helpers for the documentation diagrams, built on the vixl CLI.

Everything is plain vixl operations (rounded rectangles, paths, text), so each
diagram is an ordinary layered document before it is exported to SVG.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

from PIL import ImageFont

ONYX, GRAPHITE, BODY, STEEL = "#0E0F12", "#2A2E37", "#454A57", "#6B7385"
CYAN, CYAN_DEEP, WHITE = "#4FD1FF", "#0A84B8", "#F2F4F8"
PANEL, MIST, CYAN_WASH = "#15171C", "#9AA3B5", "#0F2B36"
AMBER = "#F5B14C"  # the one warm colour: things that stall, refuse or stop
FONT_BOLD, FONT_MED = "space-grotesk-700", "space-grotesk-500"
FONT_DIR = os.path.expanduser("~/.cache/vixl/fonts")
_metrics = {}


def vixl(*args, cwd):
    r = subprocess.run(["vixl", "--json", *args], capture_output=True, text=True, cwd=cwd)
    if r.returncode:
        sys.exit(f"vixl {' '.join(args)} failed:\n{r.stderr}\n{r.stdout}")
    return r.stdout


def text_width(text, size, font=FONT_MED):
    key = (font, size)
    if key not in _metrics:
        _metrics[key] = ImageFont.truetype(os.path.join(FONT_DIR, font + ".ttf"), size)
    return _metrics[key].getlength(text)


def wrap(text, size, width, font=FONT_MED):
    lines, cur = [], ""
    for word in text.split():
        trial = (cur + " " + word).strip()
        if cur and text_width(trial, size, font) > width:
            lines.append(cur)
            cur = word
        else:
            cur = trial
    return lines + [cur] if cur else lines


def slim(path):
    """Drop the embedded document copy and per-build layer ids (so a rebuild is byte-identical) and trim coordinates."""
    with open(path, encoding="utf-8") as fh:
        svg = fh.read()
    svg = re.sub(r'<metadata id="vixl-document">.*?</metadata>', "", svg, flags=re.S)
    svg = re.sub(r' data-vixl-id="[^"]*"', "", svg)
    def trim(m):
        v = float(m.group())
        return f"{v:.2f}".rstrip("0").rstrip(".") if abs(v) >= 1 else f"{v:.6f}".rstrip("0").rstrip(".")

    svg = re.sub(r"-?\d+\.\d{3,}", trim, svg)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(svg)


class Diagram:
    """One panel: a title block, then boxes, groups, arrows and notes placed in pixels."""

    def __init__(self, name, width, height, title, caption=None):
        self.name, self.w, self.h = name, width, height
        self.ops, self.n = [], 0
        self.rect(0, 0, width, height, ONYX, radius=22, stroke=BODY, stroke_width=1.5)
        self.mark(34, 30, 34)
        self.text(82, 52, title, 26, WHITE, FONT_BOLD)
        if caption:
            self.text(82, 76, caption, 15, MIST, FONT_MED)

    # -- primitives -----------------------------------------------------------
    def _id(self, prefix):
        self.n += 1
        return f"{prefix}-{self.n:02d}"

    def rect(self, x, y, w, h, fill, radius=10, stroke=None, stroke_width=1.5, dash=None, name=None):
        op = {"type": "shape", "shape": "rounded-rectangle", "name": name or self._id("rect"),
              "x": x, "y": y, "width": w, "height": h, "radius": radius, "fill": fill}
        if stroke:
            op.update(stroke=stroke, stroke_width=stroke_width)
        if dash:
            op["dash"] = dash
        self.ops.append(op)

    def text(self, x, baseline, text, size, color=WHITE, font=FONT_MED, align="left", tracking=0):
        width = text_width(text, size, font) + tracking * max(len(text) - 1, 0)
        left = x - width / 2 if align == "center" else x - width if align == "right" else x
        op = {"type": "text", "text": text, "name": self._id("text"), "font": font, "size": size,
              "color": color, "x": round(left, 2), "baseline_y": round(baseline, 2)}
        if tracking:
            op["tracking"] = tracking
        self.ops.append(op)

    def circle(self, cx, cy, r, fill="none", stroke=None, stroke_width=2):
        op = {"type": "shape", "shape": "ellipse", "name": self._id("dot"), "x": cx - r, "y": cy - r,
              "width": 2 * r, "height": 2 * r, "fill": fill}
        if stroke:
            op.update(stroke=stroke, stroke_width=stroke_width)
        self.ops.append(op)

    def mark(self, x, y, size):
        """The logo's nut, flat: hex body, graphite face, cyan bore."""
        import math
        c, r = size / 2, size / 2
        pts = [(x + c + r * math.cos(math.radians(60 * i)), y + c + r * math.sin(math.radians(60 * i))) for i in range(6)]
        d = "M " + " L ".join(f"{px:.2f} {py:.2f}" for px, py in pts) + " Z"
        self.ops.append({"type": "shape", "shape": "path", "name": self._id("nut"), "path": d, "x": 0, "y": 0,
                         "width": self.w, "height": self.h, "fill": BODY})
        self.circle(x + c, y + c, size * 0.42, GRAPHITE, STEEL, 1)
        self.circle(x + c, y + c, size * 0.21, "none", CYAN, max(size * 0.034 * 2, 1.5))

    # -- composites -----------------------------------------------------------
    def box(self, x, y, w, h, title, sub=None, kind="part", size=18):
        """kind: part (graphite), live (cyan edge), quiet (dark), warn (amber edge)."""
        fill, edge, ink = {"part": (GRAPHITE, STEEL, WHITE), "live": (CYAN_WASH, CYAN, WHITE),
                           "quiet": (PANEL, BODY, MIST), "warn": (GRAPHITE, AMBER, WHITE)}[kind]
        self.rect(x, y, w, h, fill, 10, edge, 1.8 if kind in ("live", "warn") else 1.5)
        lines = wrap(sub, size - 5, w - 28) if sub else []
        block = size + (len(lines) * (size - 2) if lines else 0) + (4 if lines else 0)
        top = y + (h - block) / 2
        self.text(x + w / 2, top + size * 0.82, title, size, ink, FONT_BOLD, "center")
        for i, line in enumerate(lines):
            self.text(x + w / 2, top + size + 4 + (i + 1) * (size - 2) - 3, line, size - 5, MIST if kind != "quiet" else STEEL, FONT_MED, "center")
        return (x, y, w, h)

    def group(self, x, y, w, h, label):
        self.rect(x, y, w, h, "#12141A", 14, BODY, 1.4, dash=[7, 6])
        self.text(x + 16, y + 24, label.upper(), 12, CYAN, FONT_BOLD, tracking=2.2)

    def arrow(self, pts, kind="flow", label=None, label_at=None, head=True):
        """Polyline through pts with an arrowhead at the end. kind: flow (cyan), steel, dash, warn."""
        color, width, dash = {"flow": (CYAN, 2.4, None), "steel": (MIST, 2, None),
                              "dash": (MIST, 2, [7, 6]), "warn": (AMBER, 2.4, [7, 6])}[kind]
        d = "M " + " L ".join(f"{px} {py}" for px, py in pts)
        op = {"type": "shape", "shape": "path", "name": self._id("link"), "path": d, "x": 0, "y": 0,
              "width": self.w, "height": self.h, "fill": "none", "stroke": color, "stroke_width": width,
              "line_join": "round", "line_cap": "round"}
        if dash:
            op["dash"] = dash
        if head:
            op.update(marker_end="triangle", marker_size=11)
        self.ops.append(op)
        if label:
            lx, ly = label_at or ((pts[0][0] + pts[-1][0]) / 2, (pts[0][1] + pts[-1][1]) / 2 - 8)
            self.text(lx, ly, label, 13, color if kind != "steel" else MIST, FONT_MED, "center")

    def note(self, x, baseline, text, color=MIST, size=14, align="left", width=None):
        lines = wrap(text, size, width) if width else [text]
        for i, line in enumerate(lines):
            self.text(x, baseline + i * (size + 5), line, size, color, FONT_MED, align)

    def hexbox(self, cx, cy, w, h, label, kind="part", size=15):
        """A decision: the logo's hexagon stretched to hold a question."""
        fill, edge = {"part": (GRAPHITE, STEEL), "live": (CYAN_WASH, CYAN), "warn": (GRAPHITE, AMBER)}[kind]
        k = h / 2
        pts = [(cx - w / 2 + k, cy - h / 2), (cx + w / 2 - k, cy - h / 2), (cx + w / 2, cy),
               (cx + w / 2 - k, cy + h / 2), (cx - w / 2 + k, cy + h / 2), (cx - w / 2, cy)]
        d = "M " + " L ".join(f"{px:.1f} {py:.1f}" for px, py in pts) + " Z"
        self.ops.append({"type": "shape", "shape": "path", "name": self._id("hex"), "path": d, "x": 0, "y": 0,
                         "width": self.w, "height": self.h, "fill": fill, "stroke": edge, "stroke_width": 1.8,
                         "line_join": "round"})
        lines = wrap(label, size, w - 2 * k)
        top = cy - len(lines) * (size + 3) / 2
        for i, line in enumerate(lines):
            self.text(cx, top + size + i * (size + 3) - 2, line, size, WHITE, FONT_BOLD, "center")

    def badge(self, cx, cy, n, r=15):
        self.circle(cx, cy, r, GRAPHITE, CYAN, 2)
        self.text(cx, cy + 6, str(n), 16, WHITE, FONT_BOLD, "center")

    def chip(self, x, y, label, kind="quiet", size=13):
        w = text_width(label, size, FONT_BOLD) + 22
        fill, edge, ink = {"quiet": (PANEL, BODY, MIST), "live": (CYAN_WASH, CYAN, CYAN), "warn": (PANEL, AMBER, AMBER),
                           "part": (GRAPHITE, STEEL, WHITE)}[kind]
        self.rect(x, y, w, 26, fill, 13, edge, 1.3)
        self.text(x + w / 2, y + 18, label, size, ink, FONT_BOLD, "center")
        return w

    def cell(self, x, y, w, h, label, kind="part", size=13):
        fill, edge, ink = {"part": (GRAPHITE, STEEL, WHITE), "live": (CYAN_WASH, CYAN, WHITE), "warn": (PANEL, AMBER, AMBER),
                           "quiet": (PANEL, BODY, MIST), "gone": ("#1A1410", AMBER, AMBER)}[kind]
        self.rect(x, y, w, h, fill, 6, edge, 1.4, dash=[5, 4] if kind == "gone" else None)
        self.text(x + w / 2, y + h / 2 + size * 0.36, label, size, ink, FONT_BOLD, "center")

    # -- output ---------------------------------------------------------------
    def build(self, out_dir, preview_dir=None):
        work = tempfile.mkdtemp(prefix="onyx-diagram-")
        doc = os.path.join(work, self.name + ".vixl")
        vixl("new", f"{self.w}x{self.h}", "-o", doc, "--background", "transparent", cwd=work)
        for font in (FONT_BOLD, FONT_MED):
            family, weight = "Space Grotesk", font.rsplit("-", 1)[1]
            vixl("-p", doc, "font", "install", family, "--weight", weight, cwd=work)
        ops_file = os.path.join(work, self.name + ".json")
        with open(ops_file, "w") as fh:
            json.dump(self.ops, fh)
        vixl("-p", doc, "apply", ops_file, cwd=work)
        os.makedirs(os.path.join(out_dir, "svg"), exist_ok=True)
        vixl("-p", doc, "export", os.path.join(out_dir, "svg", self.name + ".svg"), "--overwrite", "--svg-policy", "strict", cwd=work)
        slim(os.path.join(out_dir, "svg", self.name + ".svg"))
        if preview_dir:
            os.makedirs(preview_dir, exist_ok=True)
            vixl("-p", doc, "export", os.path.join(preview_dir, self.name + ".png"), "--overwrite", cwd=work)
        return doc
