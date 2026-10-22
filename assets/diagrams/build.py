"""Builds the documentation diagrams with the vixl CLI: python build.py [OUT_DIR] [NAME ...].

Each diagram is a layered Vixl document (see lib.py) exported to OUT_DIR/svg/NAME.svg, in the
logo's palette. Set ONYX_PREVIEW=DIR to also write PNG previews there. With vixl on PATH
(it downloads Space Grotesk on first run):

    python assets/diagrams/build.py
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lib import *  # noqa: E402,F403

HERE = os.path.dirname(os.path.abspath(__file__))
ARGS = sys.argv[1:]
OUT = os.path.abspath(ARGS.pop(0)) if ARGS and os.sep in ARGS[0] else HERE
ONLY = set(ARGS)
PREVIEW = os.environ.get("ONYX_PREVIEW")
DIAGRAMS = {}


def diagram(fn):
    DIAGRAMS[fn.__name__.replace("_", "-")] = fn
    return fn


@diagram
def anatomy():
    d = Diagram("anatomy", 1200, 560, "Anatomy of the engine", "One process, one model. A request goes in the intake and comes out the exhaust.")
    xs = [40 + i * 235 for i in range(5)]
    stages = [("CALLERS", "Callers", "JX Runtime, SDKs, curl. OpenAI-style JSON over HTTP", "quiet"),
              ("INTAKE", "HTTP layer", "cpp-httplib, auth, validation, chat template", "part"),
              ("FUEL RAIL", "Request queue", "FIFO; the head waits for a free slot", "part"),
              ("CYLINDERS", "Engine loop + slots", "one thread, --parallel N sequences, one batch per tick", "live"),
              ("BLOCK", "llama.cpp + ggml", "model, context, sampler, GPU or CPU kernels", "part")]
    for x, (label, title, sub, kind) in zip(xs, stages):
        d.text(x, 150, label, 12, CYAN, FONT_BOLD, tracking=2.4)
        d.box(x, 166, 190, 124, title, sub, kind, 19)
    for a, b in zip(xs, xs[1:]):
        d.arrow([(a + 190, 228), (b, 228)], "flow")
    d.box(xs[4], 380, 190, 70, "Model file", "GGUF, or safetensors converted once", "quiet", 17)
    d.arrow([(xs[4] + 95, 380), (xs[4] + 95, 290)], "steel")
    chips = ["CPU", "CUDA", "Vulkan", "Metal", "HIP", "BLAS"]
    total = sum(text_width(c, 13, FONT_BOLD) + 22 for c in chips) + 8 * (len(chips) - 1)
    px = 1160 - total
    for label in chips:
        px += d.chip(px, 478, label, "live" if label == "CPU" else "part") + 8
    d.note(1160, 530, "Pick one GPU backend at build time. CPU is always there.", MIST, 13, "right")
    d.arrow([(xs[3] + 95, 290), (xs[3] + 95, 350), (xs[0] + 95, 350), (xs[0] + 95, 290)], "flow")
    d.text((xs[0] + xs[3] + 190) / 2, 340, "EXHAUST  ·  SSE frames, usage, timings, [DONE]", 13, CYAN, FONT_BOLD, "center", 1.2)
    d.note(xs[0], 400, "What it is not: a router, a model hub or a web UI.", MIST, 14)
    d.note(xs[0], 424, "Run a second model in a second process, on its own port.", MIST, 14)
    d.note(xs[0], 478, "Binds 127.0.0.1 by default. It never reads a URL or a file on a", MIST, 14)
    d.note(xs[0], 500, "request's behalf; media arrives inline as base64.", MIST, 14)
    return d


@diagram
def request_lifecycle():
    d = Diagram("request-lifecycle", 1200, 520, "One request, start to finish", "What happens to a POST /v1/chat/completions between the socket and the last SSE frame.")
    steps = [("Authenticate", "Bearer token unless /health. Any Origin header is refused with 403."),
             ("Parse and validate", "Unsupported fields get a 400 instead of being ignored."),
             ("Render the prompt", "Jinja chat template, tools, grammar or JSON schema."),
             ("Tokenize", "Special tokens parse only in template text, never in message content."),
             ("Queue and admit", "FIFO. Pick the idle slot with the longest common prefix."),
             ("Prefill and decode", "Prompt chunks and next-token rows share one llama_batch."),
             ("Sample and stream", "Own sampler per slot. Deltas, finish chunk, usage frame, [DONE].")]
    pos = [(40 + i * 290, 140) for i in range(4)] + [(40 + i * 290, 340) for i in range(3)]
    for i, ((x, y), (title, sub)) in enumerate(zip(pos, steps)):
        d.box(x, y, 255, 120, title, sub, "live" if i in (4, 5) else "part", 18)
        d.badge(x + 24, y - 2, i + 1)
    for i in range(3):
        d.arrow([(pos[i][0] + 255, 200), (pos[i + 1][0], 200)], "flow")
    d.arrow([(pos[3][0] + 127, 260), (pos[3][0] + 127, 300), (167, 300), (167, 340)], "flow")
    for i in (4, 5):
        d.arrow([(pos[i][0] + 255, 400), (pos[i + 1][0], 400)], "flow")
    d.note(900, 372, "A disconnected client cancels the", MIST, 14, "left")
    d.note(900, 394, "request at the next tick and frees", MIST, 14, "left")
    d.note(900, 416, "its slot. Other slots never notice.", MIST, 14, "left")
    return d


@diagram
def batching_tick():
    d = Diagram("batching-tick", 1200, 590, "One tick of the engine loop", "Every slot that has work contributes rows to a single llama_batch. One decode serves them all.")
    d.box(40, 120, 300, 60, "Request queue", "FIFO, head waits when all slots are busy", "part", 17)
    d.group(24, 210, 332, 340, "Slots, one sequence each")
    rows = [("slot 0", "prefilling a prompt chunk", "live"), ("slot 1", "generating", "part"),
            ("slot 2", "generating", "part"), ("slot 3", "idle", "quiet")]
    for i, (t, s, k) in enumerate(rows):
        d.box(40, 240 + i * 74, 300, 60, t + "  ·  " + s, None, k, 16)
    d.arrow([(190, 180), (190, 210)], "dash", head=False)
    d.text(205, 203, "admit at the top of each tick", 12, MIST, FONT_MED)
    d.group(410, 210, 360, 232, "One llama_batch")
    cells = [("seq 0  ·  prompt chunk", "live"), ("seq 1  ·  one next-token row", "part"), ("seq 2  ·  one next-token row", "part")]
    for i, (t, k) in enumerate(cells):
        d.cell(430, 240 + i * 74, 320, 60, t, k, 15)
        d.arrow([(340, 270 + i * 74), (430, 270 + i * 74)], "flow")
    d.note(430, 472, "Rows carry their sequence id and the index of the", MIST, 13)
    d.note(430, 491, "logits row to read (i_batch). Idle slots add nothing.", MIST, 13)
    d.box(850, 240, 310, 100, "llama_decode", "one call per tick, in n_batch views; halves the view and retries on failure", "live", 19)
    d.box(850, 394, 310, 100, "Sample each slot", "its own sampler reads its own logits row; grammar and penalties apply here", "part", 19)
    d.arrow([(770, 326), (810, 326), (810, 290), (850, 290)], "flow")
    d.arrow([(1005, 340), (1005, 394)], "flow")
    d.arrow([(1005, 494), (1005, 570), (190, 570), (190, 550)], "dash")
    d.text(600, 560, "sampled tokens become the next tick's rows, and visible text goes to each request's stream", 12, MIST, FONT_MED, "center")
    return d


@diagram
def context_slots():
    d = Diagram("context-slots", 1200, 460, "Slots divide the context", "kv_unified stays off, so -c is split evenly. A prompt has to fit one slot, not the whole context.")
    d.text(40, 150, "-c 8192  -np 1", 15, CYAN, FONT_BOLD)
    d.cell(40, 164, 1120, 56, "slot 0  ·  8192 tokens", "live", 16)
    d.text(40, 280, "-c 8192  -np 4", 15, CYAN, FONT_BOLD)
    for i in range(4):
        d.cell(40 + i * 280, 294, 272, 56, f"slot {i}  ·  2048 tokens", "part", 16)
    d.text(40, 392, "GET /props", 13, CYAN, FONT_BOLD, tracking=1.5)
    d.note(40, 414, "n_ctx = 2048 is the per-slot window: size requests against it.   n_ctx_total = 8192 is informational.", MIST, 14)
    d.note(40, 436, "Raise -c when you raise -np, or accept smaller windows per request.", MIST, 14)
    return d


@diagram
def boot_sequence():
    d = Diagram("boot-sequence", 1200, 500, "Boot sequence", "main.cpp, top to bottom. The port stays closed until the model is loaded.")
    xs = [34 + i * 234 for i in range(5)]
    steps = [("Parse arguments", "args.cpp. An unknown flag exits 1 with the flag named.", "part"),
             ("Resolve the model", "convert.cpp. GGUF passes through; safetensors converts once.", "part"),
             ("Load", "engine.cpp. Model, context, optional mtmd, one slot per --parallel.", "live"),
             ("Bind and listen", "server.cpp. Routes registered, 127.0.0.1:8080 unless told otherwise.", "part"),
             ("Serve", "Until signalled. Then the backend is freed and the process exits.", "part")]
    for i, (x, (t, s, k)) in enumerate(zip(xs, steps)):
        d.box(x, 150, 196, 124, t, s, k, 18)
        d.badge(x + 24, 148, i + 1)
    for a, b in zip(xs, xs[1:]):
        d.arrow([(a + 196, 212), (b, 212)], "flow")
    d.text(xs[1], 330, "RESOLVE", 12, CYAN, FONT_BOLD, tracking=2.2)
    d.note(xs[1], 352, "Not GGUF and not a safetensors", MIST, 13)
    d.note(xs[1], 371, "directory: startup error.", MIST, 13)
    d.text(xs[2], 330, "LOAD", 12, CYAN, FONT_BOLD, tracking=2.2)
    d.note(xs[2], 352, "--embedding: one slot, no engine thread. Decision model: one request at a time. --mmproj: cache reuse and shift go off.", MIST, 13, width=215)
    d.box(xs[3], 330, 430, 70, "A 200 from GET /health means the model is loaded", "Nothing listens earlier, so early connections are refused rather than answered.", "quiet", 16)
    return d


@diagram
def safetensors_conversion():
    d = Diagram("safetensors-conversion", 1200, 650, "Safetensors resolution", "convert.cpp decides what -m means before the engine loads anything.")
    d.box(40, 130, 170, 60, "-m PATH", None, "quiet", 17)
    d.hexbox(330, 160, 200, 72, ".gguf or GGUF magic?")
    d.arrow([(210, 160), (230, 160)], "flow", head=True)
    d.box(540, 130, 230, 60, "Load it unchanged", None, "live", 17)
    d.arrow([(430, 160), (540, 160)], "flow", "yes", (485, 150))
    d.hexbox(330, 300, 220, 72, "config.json and *.safetensors?")
    d.arrow([(330, 196), (330, 264)], "flow", "no", (350, 236))
    d.box(540, 270, 230, 60, "Startup error", None, "warn", 17)
    d.arrow([(440, 300), (540, 300)], "warn", "no", (490, 290))
    d.hexbox(330, 440, 200, 72, "Cache fresh?")
    d.arrow([(330, 336), (330, 404)], "flow", "yes", (352, 376))
    d.box(540, 410, 230, 60, "Load the cached GGUF", None, "live", 17)
    d.arrow([(430, 440), (540, 440)], "flow", "yes", (485, 430))
    d.box(215, 545, 230, 70, "Convert", "python3 convert-safetensors.py, f16, numpy only", "part", 17)
    d.arrow([(330, 476), (330, 545)], "flow", "no", (350, 516))
    d.box(540, 545, 230, 70, "Rename into the cache", "atomic, from a unique temp file", "part", 17)
    d.arrow([(445, 580), (540, 580)], "flow")
    d.arrow([(655, 545), (655, 470)], "flow")
    d.group(830, 120, 330, 500, "Rules of the cache")
    d.note(852, 168, "Path", CYAN, 13)
    d.note(852, 188, "<convert-dir or model dir>/", MIST, 13)
    d.note(852, 207, "<name>-<path hash>-converter-v2-f16.gguf", MIST, 12)
    d.note(852, 252, "Fresh means", CYAN, 13)
    d.note(852, 272, "valid GGUF magic, large enough, and newer", MIST, 13)
    d.note(852, 291, "than the weights and the config files.", MIST, 13)
    d.note(852, 336, "Handles", CYAN, 13)
    d.note(852, 356, "Hugging Face LlamaForCausalLM, sharded or", MIST, 13)
    d.note(852, 375, "not, with a byte-level BPE tokenizer.json.", MIST, 13)
    d.note(852, 420, "Refuses, with a named reason", CYAN, 13)
    d.note(852, 440, "Other architectures, SentencePiece, exotic", MIST, 13)
    d.note(852, 459, "dtypes, a missing tensor, or a vocab mismatch.", MIST, 13)
    d.note(852, 504, "Locate the script", CYAN, 13)
    d.note(852, 524, "$ONYX_ENGINE_CONVERT_SCRIPT, then ../scripts/", MIST, 13)
    d.note(852, 543, "and ../../scripts/ beside the binary.", MIST, 13)
    return d


@diagram
def kv_reuse_and_shift():
    d = Diagram("kv-reuse-and-shift", 1200, 640, "Two ways a slot saves work", "Prefix reuse skips tokens it already holds. Context shift makes room when the slot fills.")
    d.group(24, 110, 1152, 214, "Prefix reuse  ·  --cache-reuse N")
    d.text(48, 190, "slot cache", 14, MIST)
    d.cell(190, 168, 200, 38, "system prompt", "part"); d.cell(394, 168, 150, 38, "turn 1", "part"); d.cell(548, 168, 150, 38, "reply 1", "part")
    d.text(48, 250, "new prompt", 14, MIST)
    d.cell(190, 228, 200, 38, "system prompt", "live"); d.cell(394, 228, 150, 38, "turn 1", "live"); d.cell(548, 228, 150, 38, "reply 1", "live")
    d.cell(702, 228, 190, 38, "turn 2  (new)", "warn")
    d.note(190, 296, "Longest common prefix, kept at least one token short. Cyan is reused, amber is decoded.", MIST, 13)
    d.note(740, 188, "One previous prompt per slot. No fuzzy matching,", MIST, 13)
    d.note(740, 207, "no RAM tier. Not applied to embeddings.", MIST, 13)
    d.group(24, 350, 1152, 266, "Context shift  ·  --context-shift  --keep N")
    d.text(48, 430, "slot full", 14, MIST)
    d.cell(190, 408, 180, 38, "keep  (n_keep)", "live"); d.cell(374, 408, 330, 38, "oldest half of the rest", "gone"); d.cell(708, 408, 330, 38, "recent tokens", "part")
    d.text(48, 500, "after shift", 14, MIST)
    d.cell(190, 478, 180, 38, "keep  (n_keep)", "live"); d.cell(374, 478, 330, 38, "recent tokens, positions moved down", "part"); d.cell(708, 478, 330, 38, "room to keep generating", "quiet")
    d.arrow([(540, 446), (540, 478)], "dash", head=True)
    d.note(190, 556, "1  llama_memory_seq_rm drops the window.   2  llama_memory_seq_add slides what follows.   3  The result reports truncated.", MIST, 13)
    d.note(190, 578, "Without the flag a full slot finishes with finish_reason length. A projector (--mmproj) turns both features off.", MIST, 13)
    return d


@diagram
def reasoning_budget():
    d = Diagram("reasoning-budget", 1200, 540, "Reasoning budget", "A small state machine per slot, advanced after every sampled token.")
    d.chip(60, 130, "request begins", "quiet")
    d.arrow([(130, 156), (130, 178)], "steel")
    d.hexbox(150, 215, 230, 70, "budget below 0?")
    d.box(50, 340, 200, 84, "OFF", "never leaves; costs nothing", "quiet", 18)
    d.arrow([(150, 250), (150, 340)], "flow", "yes", (172, 300))
    d.hexbox(465, 215, 260, 70, "prompt already inside the think block?")
    d.arrow([(265, 215), (335, 215)], "flow", "no", (300, 205))
    d.box(640, 175, 190, 80, "IDLE", "scan for the start tag", "part", 18)
    d.arrow([(595, 215), (640, 215)], "flow", "no", (618, 205))
    d.box(900, 175, 190, 80, "COUNTING", "count tokens, watch for the end tag", "live", 18)
    d.arrow([(830, 215), (900, 215)], "flow", "start tag", (865, 203))
    d.arrow([(465, 180), (465, 140), (995, 140), (995, 175)], "dash")
    d.text(730, 132, "yes: deepseek-style templates skip IDLE", 12, MIST, FONT_MED, "center")
    d.box(640, 345, 190, 84, "FORCING", "emit the message, then the closing tag", "warn", 18)
    d.box(900, 345, 190, 84, "DONE", "pass through, closed or forced", "part", 18)
    d.arrow([(920, 255), (920, 300), (735, 300), (735, 345)], "warn")
    d.text(830, 292, "budget reached", 12, AMBER, FONT_MED, "center")
    d.arrow([(1040, 255), (1040, 345)], "flow")
    d.text(1050, 306, "end tag seen", 12, CYAN, FONT_MED)
    d.arrow([(830, 387), (900, 387)], "flow")
    d.note(60, 470, "--reasoning-budget N or reasoning_budget_tokens per request.  -1 off, 0 closes thinking on the first token, N allows N tokens.", MIST, 13)
    d.note(60, 492, "The reasoning and content split in the response is still whatever the template's own parser makes of the final text.", MIST, 13)
    return d


@diagram
def multimodal_path():
    d = Diagram("multimodal-path", 1200, 560, "Media, from request to KV cache", "Images and audio travel inline. The engine never fetches anything for you.")
    pos = [(40, 140), (440, 140), (840, 140), (40, 340), (440, 340), (840, 340)]
    steps = [("Request", "image_url or input_audio parts, as data: URIs or bare base64", "part"),
             ("extract_media()", "decode; http, https and file:// are a 400; the part becomes a marker", "part"),
             ("Render the template", "one marker per buffer, in order, inside the prompt text", "part"),
             ("Queue and admit", "media prompts never reuse KV; n_past starts at 0", "part"),
             ("prefill_media()", "mtmd encodes, then decodes the chunks itself", "warn"),
             ("Generate", "back in the shared batch with every other slot", "live")]
    for i, ((x, y), (t, s, k)) in enumerate(zip(pos, steps)):
        d.box(x, y, 320, 124, t, s, k, 19)
        d.badge(x + 24, y - 2, i + 1)
    d.arrow([(360, 202), (440, 202)], "flow")
    d.arrow([(760, 202), (840, 202)], "flow")
    d.arrow([(1000, 264), (1000, 300), (200, 300), (200, 340)], "flow")
    d.arrow([(360, 402), (440, 402)], "flow")
    d.arrow([(760, 402), (840, 402)], "flow")
    d.text(600, 332, "STALL", 12, AMBER, FONT_BOLD, tracking=2.2)
    d.note(440, 492, "Step 5 runs outside the shared batch and holds the engine loop for that one prefill.", AMBER, 14)
    d.note(440, 514, "n_past comes from mtmd, because M-RoPE does not advance one position per token.", MIST, 13)
    d.note(40, 492, "GET /props reports", MIST, 13)
    d.note(40, 511, "modalities.vision and .audio.", MIST, 13)
    return d


@diagram
def decision_models():
    d = Diagram("decision-models", 1200, 720, "Decision models on /v1/systemone", "A typed answer from one prefill per question. No token is sampled.")
    pos = [(40, 130), (440, 130), (840, 130), (40, 320), (440, 320), (840, 320)]
    steps = [("Request", "a state, plus choice, score and noul questions; images with --mmproj", "part"),
             ("Render each question", "the model's own systemone chat template, special tokens parsed", "part"),
             ("Prefill once", "questions share their prefix: seq 0 evaluated once, seq 1 copies it", "live"),
             ("Read the outputs", "the family decides where the answer sits in the batch", "part"),
             ("Softmax", "at the family's stored temperature, averaged across variants", "part"),
             ("Answers", "per question: the pick, probabilities, confidence", "part")]
    for i, ((x, y), (t, s, k)) in enumerate(zip(pos, steps)):
        d.box(x, y, 320, 124, t, s, k, 19)
        d.badge(x + 24, y - 2, i + 1)
    d.arrow([(360, 192), (440, 192)], "flow")
    d.arrow([(760, 192), (840, 192)], "flow")
    d.arrow([(1000, 254), (1000, 290), (200, 290), (200, 320)], "flow")
    d.arrow([(360, 382), (440, 382)], "flow")
    d.arrow([(760, 382), (840, 382)], "flow")
    d.text(40, 494, "WHERE THE ANSWER IS READ", 12, CYAN, FONT_BOLD, tracking=2.2)
    rows = [("label logits at the last token", ["openjev", "lev", "nimble", "pplx-decider", "granite-switch", "lfm2-d1", "h2o-lightning"]),
            ("one embeddings row per option", ["laya", "lfm2-d1-omni"]),
            ("hidden-state dot products", ["kev"]),
            ("a joint head over marked spans", ["clef", "vllm-decision2"]),
            ("non-causal, whole prompt in one batch", ["vllm-d3"])]
    for i, (label, names) in enumerate(rows):
        yy = 512 + i * 38
        d.text(40, yy + 18, label, 13, MIST)
        px = 330
        for n in names:
            px += d.chip(px, yy, n, "part") + 6
    return d


@diagram
def jx_runtime():
    d = Diagram("jx-runtime", 1200, 580, "How JX Runtime drives the engine", "One onyx-engine process per loaded model, each on its own port.")
    d.box(40, 130, 200, 90, "Your apps", "agents, SDKs, UIs", "quiet", 18)
    d.box(330, 130, 540, 90, "JX Runtime", "Node.js. Model registry, adapters, SSE usage tap.", "live", 21)
    d.box(960, 130, 200, 90, "GitHub Releases", "onyx-engine archives", "quiet", 17)
    d.arrow([(240, 175), (330, 175)], "flow", "requests", (285, 165))
    d.arrow([(960, 175), (870, 175)], "dash", "install", (915, 165))
    d.arrow([(600, 220), (600, 270)], "flow", head=False)
    d.arrow([(225, 270), (975, 270)], "flow", head=False)
    kids = [(225, "onyx-engine  :8081", "chat model, -np 2", "--alias chat"), (600, "onyx-engine  :8082", "embedding model", "--embedding"), (975, "onyx-engine  :8083", "decision model", "serves /v1/systemone")]
    for cx, t, s1, s2 in kids:
        d.arrow([(cx, 270), (cx, 340)], "flow")
        d.box(cx - 165, 340, 330, 110, t, s1 + ".  " + s2, "part", 19)
    d.text(600, 262, "SPAWN  ·  POLL /health  ·  PROXY /v1/*", 12, CYAN, FONT_BOLD, "center", 1.6)
    d.note(40, 492, "It probes --help before passing any flag, reads the per-slot n_ctx from /props,", MIST, 14)
    d.note(40, 514, "and reads usage and timings from the last SSE frame. Install path: <data-dir>/engines/onyx-engine/.", MIST, 14)
    d.note(40, 548, "The engine does not know JX Runtime exists. It answers HTTP and nothing else.", AMBER, 14)
    return d


@diagram
def build_layers():
    d = Diagram("build-layers", 1200, 620, "What the binary is made of", "CMake builds only the libraries onyx-engine links. None of llama.cpp's own tools or server.")
    d.box(60, 130, 460, 78, "onyx-engine", "src/*.cpp, plus its own onyx-httplib", "live", 20)
    d.box(60, 252, 220, 78, "llama-common", "chat, sampling, JSON, grammar", "part", 18)
    d.box(300, 252, 220, 78, "mtmd", "image and audio encoders", "part", 18)
    d.box(60, 374, 460, 78, "llama", "model loading, context, batched decode", "part", 20)
    d.box(60, 496, 460, 78, "ggml", "tensors and kernels", "part", 20)
    d.arrow([(290, 208), (170, 252)], "flow"); d.arrow([(290, 208), (410, 252)], "flow")
    d.arrow([(170, 330), (170, 374)], "flow"); d.arrow([(410, 330), (410, 374)], "flow")
    d.arrow([(290, 452), (290, 496)], "flow")
    d.group(580, 120, 580, 190, "Where the llama.cpp source comes from  ·  first match wins")
    srcs = ["-DONYX_ENGINE_LLAMA_DIR=<path>", "node_modules/@jxburros/llama-cpp-source", "vendor/llama.cpp  (manual, gitignored)"]
    for i, s in enumerate(srcs):
        d.badge(612, 170 + i * 44, i + 1, 13)
        d.text(640, 176 + i * 44, s, 15, WHITE, FONT_MED)
    d.group(580, 335, 580, 239, "What the build forces")
    d.text(604, 392, "OFF", 13, AMBER, FONT_BOLD, tracking=2)
    d.note(660, 392, "tests, examples, tools, server, app, OpenSSL,", MIST, 14)
    d.note(660, 412, "MTMD_VIDEO (it would shell out to ffmpeg)", MIST, 14)
    d.text(604, 458, "ON", 13, CYAN, FONT_BOLD, tracking=2)
    d.note(660, 458, "common library, mtmd as a standalone library", MIST, 14)
    d.text(604, 516, "GPU", 13, CYAN, FONT_BOLD, tracking=2)
    d.note(660, 516, "ONYX_ENGINE_CUDA, VULKAN, METAL, HIP or BLAS", MIST, 14)
    d.note(660, 536, "forward to the matching GGML_* variable.", MIST, 14)
    return d


@diagram
def vendor_pipeline():
    d = Diagram("vendor-pipeline", 1200, 440, "From pin to binary", "The build uses an exact llama.cpp commit, fetched and verified with Git. No registry package is involved.")
    xs = [34 + i * 234 for i in range(5)]
    steps = [("The pin", "llama.cpp.conf holds one 40-hex commit", "quiet"),
             ("npm ci", "postinstall runs scripts/install-vendor.js", "part"),
             ("Fetch and verify", "sparse git fetch; object hashes prove the commit", "part"),
             ("node_modules", "@jxburros/llama-cpp-source, stamped with the commit", "part"),
             ("CMake and build", "build/onyx-engine, pin string baked in", "live")]
    for i, (x, (t, s, k)) in enumerate(zip(xs, steps)):
        d.box(x, 140, 196, 124, t, s, k, 18)
    for a, b in zip(xs, xs[1:]):
        d.arrow([(a + 196, 202), (b, 202)], "flow")
    d.text(34, 322, "PINNED NOW", 12, CYAN, FONT_BOLD, tracking=2.2)
    d.note(34, 346, "llama.cpp b11820 (d77256999), package 0.7.0-b11820.gd77256999.", MIST, 14)
    d.note(34, 368, "ggml f1aa6a3, package 0.27.0-gf1aa6a346. It follows the llama.cpp pin:", MIST, 14)
    d.note(34, 390, "after a bump, scripts/check-ggml-pin.sh prints ok or the commit to move to.", MIST, 14)
    d.note(34, 420, "Offline? npm run vendor -- /path/to/llama.cpp reuses a checkout you already have.", MIST, 14)
    return d


@diagram
def release_pipeline():
    d = Diagram("release-pipeline", 1200, 500, "Release pipeline", "Manual, from main, dry run first. Pushing a tag no longer publishes anything.")
    xs = [34 + i * 234 for i in range(5)]
    steps = [("Run workflow", "Actions, Release. Stable or preview. dry_run defaults to on.", "quiet"),
             ("Plan", "selects apps whose version is ahead of what is published", "part"),
             ("Build x4", "static Release binaries, --version as the sanity check", "live"),
             ("Stage", "archives, checksums.txt, complete inventory", "part"),
             ("Finish", "verified draft publish, with approvals if configured", "part")]
    for x, (t, s, k) in zip(xs, steps):
        d.box(x, 140, 196, 124, t, s, k, 19)
    for a, b in zip(xs, xs[1:]):
        d.arrow([(a + 196, 202), (b, 202)], "flow")
    d.text(xs[2], 306, "PLATFORM KEYS", 12, CYAN, FONT_BOLD, tracking=2.2)
    px = xs[2]
    for k in ("linux-x64", "darwin-arm64", "darwin-x64", "win32-x64"):
        px += d.chip(px, 316, k, "part") + 6
    d.note(34, 380, "All or nothing: one failed platform blocks the release, so a partial set never ships.", MIST, 14)
    d.note(34, 402, "The asset names are a contract: JX Runtime builds them from process.platform and process.arch.", MIST, 14)
    d.note(34, 424, "Published versions are never overwritten. A correction is a new version.", MIST, 14)
    d.note(34, 462, "Every change to src, scripts, packaging or CMakeLists.txt must advance the version, or validation fails.", AMBER, 14)
    return d


@diagram
def source_map():
    d = Diagram("source-map", 1200, 560, "Where the parts live", "Six files do the work. Line counts are as of 0.5.0.")
    d.box(500, 130, 200, 80, "main.cpp", "73 lines. Wires the rest.", "live", 19)
    cols = [(60, "args.cpp", "216 lines. argv, --help, --version", "part"), (330, "convert.cpp", "389 lines. GGUF or safetensors", "part"),
            (600, "engine.cpp", "1,559 lines. Model, slots, batching loop", "live"), (870, "server.cpp", "1,415 lines. HTTP, routes, SSE", "part")]
    for x, t, s, k in cols:
        d.box(x, 290, 230, 96, t, s, k, 19)
    d.arrow([(600, 210), (600, 250)], "flow", head=False)
    d.arrow([(175, 250), (985, 250)], "flow", head=False)
    for x, *_ in cols:
        d.arrow([(x + 115, 250), (x + 115, 290)], "flow")
    d.box(735, 450, 240, 80, "decision.cpp", "1,708 lines. /v1/systemone", "part", 19)
    d.arrow([(985, 386), (985, 418), (905, 418), (905, 450)], "flow")
    d.arrow([(715, 386), (715, 418), (805, 418), (805, 450)], "flow")
    d.arrow([(870, 338), (830, 338)], "steel")
    d.text(60, 440, "WHO CALLS WHOM", 12, CYAN, FONT_BOLD, tracking=2.2)
    d.note(60, 464, "server calls engine for generate, embed, decide, tokenize.", MIST, 14)
    d.note(60, 486, "engine calls decision to render and score questions.", MIST, 14)
    d.note(60, 508, "convert shells out to scripts/convert-safetensors.py.", MIST, 14)
    return d


@diagram
def route_modes():
    d = Diagram("route-modes", 1200, 560, "Which routes answer in which mode", "The mode is fixed at launch by the model and --embedding. Anything else answers 501.")
    cols = [(400, "text model", "default"), (650, "embedding", "--embedding"), (900, "decision model", "<arch>.decision.type")]
    for x, t, s in cols:
        d.text(x + 115, 148, t, 18, WHITE, FONT_BOLD, "center")
        d.text(x + 115, 170, s, 13, CYAN, FONT_MED, "center")
    rows = [("Probes and text utilities", "/health /props /v1/models /tokenize /detokenize /apply-template", "yyy"),
            ("Chat and text completions", "/v1/chat/completions /v1/completions", "yxx"),
            ("Embeddings", "/v1/embeddings", "xyx"),
            ("Typed decisions", "/v1/systemone", "xxy")]
    for i, (name, routes, marks) in enumerate(rows):
        y = 196 + i * 84
        d.rect(40, y, 1120, 72, PANEL, 10, BODY, 1.2)
        d.text(58, y + 26, name, 17, WHITE, FONT_BOLD)
        d.note(58, y + 46, routes, MIST, 12, width=320)
        for (x, *_), m in zip(cols, marks):
            if m == "y":
                d.cell(x, y + 14, 230, 44, "answers", "live", 15)
            else:
                d.cell(x, y + 14, 230, 44, "501", "warn", 15)
    return d


@diagram
def roadmap_map():
    d = Diagram("roadmap-map", 1200, 640, "Roadmap at a glance", "Now, next and later. ROADMAP.md has the detail, the owners and the decisions still open.")
    cols = [(40, "NOW", "live", ["logit_bias for label tokens (optional)", "Decision images for clef, lfm2-d1-omni, vllm-d3", "--cache-type-k/-v and MoE offload flags", "/metrics, X-Request-Id, --max-queue", "Hardening: Host allowlist, key compare, payload cap", "Stage 1 of the common/ plan", "Dry run of the unified release workflow"]),
            (430, "NEXT: WEEKS", "part", ["Pin-bump pipeline", "KV state: checkpoints and a host-RAM cache", "Auto-fit (--fit) for one GPU plus CPU", "Embeddings base64, /v1/rerank, reasoning_effort", "/v1/responses and /v1/messages", "Sanitizers, fuzzing, attestations, arm64", "Release convention for installers"]),
            (820, "LATER", "quiet", ["Own the chat layer (Stage 3)", "Speculative decoding (Stage 4)", "Converter: Llama 3 RoPE, llama-bpe, -hf", "503 while loading, with JX Runtime", "Unified KV behind a flag", "Waiting on upstream: MTP, EAGLE3, DFlash"])]
    for x, title, kind, items in cols:
        fill, edge, _ = {"live": (CYAN_WASH, CYAN, 0), "part": (GRAPHITE, STEEL, 0), "quiet": (PANEL, BODY, 0)}[kind]
        d.rect(x, 124, 340, 420, fill, 14, edge, 1.6)
        d.text(x + 22, 160, title, 14, CYAN if kind != "quiet" else MIST, FONT_BOLD, tracking=2.4)
        yy = 198
        for item in items:
            lines = wrap(item, 15, 268)
            d.circle(x + 28, yy - 5, 4, "none", CYAN if kind != "quiet" else STEEL, 1.6)
            for j, line in enumerate(lines):
                d.text(x + 44, yy + j * 19, line, 15, WHITE if kind != "quiet" else MIST, FONT_MED)
            yy += len(lines) * 19 + 28
    d.note(40, 590, "Not planned: multi-model routing, built-in tools or MCP, a web UI, TTS or realtime. JX Runtime already does the first two.", AMBER, 14)
    return d



def run():
    for name, fn in DIAGRAMS.items():
        if ONLY and name not in ONLY:
            continue
        fn().build(OUT, PREVIEW)
        print("built", name)


run()
