#!/usr/bin/env python3
"""Builds a tiny random-weight CLIP/llava mmproj GGUF for smoke testing.

Pairs with scripts/make-tiny-model.py: the projector's output dimension must
match that model's embedding length so mtmd can splice image embeddings into
the text model. Generates gibberish, but exercises the real mtmd/clip load,
preprocess and encode path offline.

Usage: python3 scripts/make-tiny-mmproj.py [output.gguf]
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

_CANDIDATES = [
    REPO / "node_modules" / "@jxburros" / "llama-cpp-source",
    REPO / "vendor" / "llama.cpp",
]
try:
    LLAMA_SRC = next(p for p in _CANDIDATES if (p / "CMakeLists.txt").exists())
except StopIteration:
    sys.exit("error: llama.cpp source tree not found - run: npm ci")

sys.path.insert(0, str(LLAMA_SRC / "gguf-py"))

import numpy as np  # noqa: E402
import gguf  # noqa: E402

# must match scripts/make-tiny-model.py
TEXT_N_EMBD = 64

IMAGE_SIZE = 32
PATCH_SIZE = 16          # -> 2x2 = 4 patches
V_N_EMBD   = 32
V_N_HEAD   = 2
V_N_LAYER  = 2
V_N_FF     = 128

N_PATCHES = (IMAGE_SIZE // PATCH_SIZE) ** 2


def main() -> None:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "models-test" / "tiny-mmproj-random.gguf"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    w = gguf.GGUFWriter(str(out_path), "clip")
    w.add_name("jx-engine-tiny-mmproj")
    w.add_description("tiny random clip vision encoder for jx-engine smoke tests")
    w.add_file_type(gguf.LlamaFileType.ALL_F32)

    w.add_bool("clip.has_vision_encoder", True)
    w.add_bool("clip.has_audio_encoder", False)
    w.add_string("clip.projector_type", "mlp")
    w.add_bool("clip.use_gelu", True)

    w.add_uint32("clip.vision.embedding_length", V_N_EMBD)
    w.add_uint32("clip.vision.feed_forward_length", V_N_FF)
    w.add_uint32("clip.vision.block_count", V_N_LAYER)
    w.add_uint32("clip.vision.attention.head_count", V_N_HEAD)
    w.add_uint32("clip.vision.projection_dim", TEXT_N_EMBD)
    w.add_float32("clip.vision.attention.layer_norm_epsilon", 1e-5)
    w.add_uint32("clip.vision.image_size", IMAGE_SIZE)
    w.add_uint32("clip.vision.patch_size", PATCH_SIZE)
    w.add_array("clip.vision.image_mean", [0.5, 0.5, 0.5])
    w.add_array("clip.vision.image_std", [0.5, 0.5, 0.5])

    rng = np.random.default_rng(1234)

    def t(name: str, shape: tuple) -> None:
        w.add_tensor(name, rng.standard_normal(size=shape, dtype=np.float32) * 0.02)

    # patch embedding conv kernel: ne = [patch, patch, 3, n_embd]
    t("v.patch_embd.weight", (V_N_EMBD, 3, PATCH_SIZE, PATCH_SIZE))
    t("v.patch_embd.bias", (V_N_EMBD,))
    t("v.position_embd.weight", (N_PATCHES, V_N_EMBD))
    t("v.pre_ln.weight", (V_N_EMBD,))
    t("v.pre_ln.bias", (V_N_EMBD,))
    t("v.post_ln.weight", (V_N_EMBD,))
    t("v.post_ln.bias", (V_N_EMBD,))

    for il in range(V_N_LAYER):
        p = f"v.blk.{il}."
        for proj in ("attn_q", "attn_k", "attn_v", "attn_out"):
            t(p + proj + ".weight", (V_N_EMBD, V_N_EMBD))
            t(p + proj + ".bias", (V_N_EMBD,))
        for ln in ("ln1", "ln2"):
            t(p + ln + ".weight", (V_N_EMBD,))
            t(p + ln + ".bias", (V_N_EMBD,))
        t(p + "ffn_up.weight", (V_N_FF, V_N_EMBD))
        t(p + "ffn_up.bias", (V_N_FF,))
        t(p + "ffn_down.weight", (V_N_EMBD, V_N_FF))
        t(p + "ffn_down.bias", (V_N_EMBD,))

    # llava MLP projector: vision embedding -> text embedding. Both layers are
    # required: clip_n_mmproj_embd() reads the output dimension off mm.2.
    # (mm.1/mm.3 are deliberately absent - their presence would reclassify the
    # projector as the Yi-style "mlp_norm" variant.)
    t("mm.0.weight", (TEXT_N_EMBD, V_N_EMBD))
    t("mm.0.bias", (TEXT_N_EMBD,))
    t("mm.2.weight", (TEXT_N_EMBD, TEXT_N_EMBD))
    t("mm.2.bias", (TEXT_N_EMBD,))

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    print(f"wrote {out_path} ({out_path.stat().st_size / 1e6:.2f} MB)")


if __name__ == "__main__":
    main()
