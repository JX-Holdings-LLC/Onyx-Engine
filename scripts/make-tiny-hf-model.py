#!/usr/bin/env python3
"""Builds a tiny HF-format (safetensors) llama-architecture model directory
for exercising onyx-engine's safetensors -> GGUF conversion path end to end.

Unlike scripts/make-tiny-model.py (which writes a GGUF directly), this script
produces the *source* format onyx-engine's `-m <path>` conversion feature
consumes: a directory with config.json, a tokenizer, and model.safetensors.
It depends on numpy only - no torch, no transformers, no safetensors package -
matching the same constraint placed on scripts/convert-safetensors.py, the
converter this model is meant to feed.

The tokenizer is a minimal from-scratch byte-level BPE vocabulary (standard
GPT-2 byte<->unicode mapping, no merges beyond byte tokens, three special
tokens). It round-trips any UTF-8 text through single-byte tokens, so
generation quality is irrelevant - this exists to exercise the conversion +
serving pipeline, not to produce a useful model.

Usage: python3 scripts/make-tiny-hf-model.py [output-dir]
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent

N_VOCAB   = 256 + 3   # 256 byte tokens + <unk>, <s>, </s>
N_EMBD    = 64
N_HEAD    = 4
N_HEAD_KV = 4
N_LAYER   = 2
N_FF      = 128
N_CTX     = 512
RMS_EPS   = 1e-5
ROPE_THETA = 10000.0

UNK_ID, BOS_ID, EOS_ID = 256, 257, 258


def bytes_to_unicode() -> dict:
    """Standard GPT-2 byte<->printable-unicode mapping (used by llama.cpp's
    BPE detokenizer to recover raw bytes from token text), reproduced here so
    generation needs nothing beyond the stdlib + numpy."""
    bs = (list(range(ord("!"), ord("~") + 1)) +
          list(range(ord("\xa1"), ord("\xac") + 1)) +
          list(range(ord("\xae"), ord("\xff") + 1)))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b)
            cs.append(256 + n)
            n += 1
    return dict(zip(bs, (chr(c) for c in cs)))


def write_config(out_dir: Path) -> None:
    config = {
        "architectures": ["LlamaForCausalLM"],
        "model_type": "llama",
        "hidden_size": N_EMBD,
        "intermediate_size": N_FF,
        "num_hidden_layers": N_LAYER,
        "num_attention_heads": N_HEAD,
        "num_key_value_heads": N_HEAD_KV,
        "vocab_size": N_VOCAB,
        "max_position_embeddings": N_CTX,
        "rms_norm_eps": RMS_EPS,
        "rope_theta": ROPE_THETA,
        "tie_word_embeddings": False,
        "bos_token_id": BOS_ID,
        "eos_token_id": EOS_ID,
        "torch_dtype": "float32",
    }
    (out_dir / "config.json").write_text(json.dumps(config, indent=2))


def write_tokenizer(out_dir: Path) -> None:
    byte_encoder = bytes_to_unicode()
    vocab = {byte_encoder[b]: b for b in range(256)}
    vocab["<unk>"] = UNK_ID
    vocab["<s>"] = BOS_ID
    vocab["</s>"] = EOS_ID

    tokenizer_json = {
        "model": {
            "type": "BPE",
            "vocab": vocab,
            "merges": [],
        },
        "added_tokens": [
            {"id": UNK_ID, "content": "<unk>", "special": True},
            {"id": BOS_ID, "content": "<s>", "special": True},
            {"id": EOS_ID, "content": "</s>", "special": True},
        ],
    }
    (out_dir / "tokenizer.json").write_text(json.dumps(tokenizer_json))

    tokenizer_config = {
        "bos_token": "<s>",
        "eos_token": "</s>",
        "unk_token": "<unk>",
        "model_max_length": N_CTX,
    }
    (out_dir / "tokenizer_config.json").write_text(json.dumps(tokenizer_config, indent=2))


def save_safetensors(path: Path, tensors: dict) -> None:
    """Hand-rolled safetensors writer: an 8-byte little-endian header length,
    a JSON header describing each tensor's dtype/shape/byte range, then the
    raw little-endian tensor bytes back to back. See
    https://github.com/huggingface/safetensors for the format description;
    this intentionally avoids the `safetensors` package."""
    header = {}
    blobs = []
    offset = 0
    for name, arr in tensors.items():
        arr = np.ascontiguousarray(arr, dtype=np.float32)
        nbytes = arr.nbytes
        header[name] = {
            "dtype": "F32",
            "shape": list(arr.shape),
            "data_offsets": [offset, offset + nbytes],
        }
        offset += nbytes
        blobs.append(arr.tobytes())

    header_bytes = json.dumps(header).encode("utf-8")
    pad = (-len(header_bytes)) % 8
    header_bytes += b" " * pad

    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(header_bytes)))
        f.write(header_bytes)
        for blob in blobs:
            f.write(blob)


def write_weights(out_dir: Path) -> None:
    rng = np.random.default_rng(1234)
    head_dim = N_EMBD // N_HEAD
    kv_dim = N_HEAD_KV * head_dim

    def w(shape):
        return (rng.standard_normal(size=shape, dtype=np.float32) * 0.02).astype(np.float32)

    tensors = {
        "model.embed_tokens.weight": w((N_VOCAB, N_EMBD)),
        "model.norm.weight": w((N_EMBD,)),
        "lm_head.weight": w((N_VOCAB, N_EMBD)),
    }
    for i in range(N_LAYER):
        p = f"model.layers.{i}."
        tensors[p + "input_layernorm.weight"] = w((N_EMBD,))
        tensors[p + "self_attn.q_proj.weight"] = w((N_EMBD, N_EMBD))
        tensors[p + "self_attn.k_proj.weight"] = w((kv_dim, N_EMBD))
        tensors[p + "self_attn.v_proj.weight"] = w((kv_dim, N_EMBD))
        tensors[p + "self_attn.o_proj.weight"] = w((N_EMBD, N_EMBD))
        tensors[p + "post_attention_layernorm.weight"] = w((N_EMBD,))
        tensors[p + "mlp.gate_proj.weight"] = w((N_FF, N_EMBD))
        tensors[p + "mlp.up_proj.weight"] = w((N_FF, N_EMBD))
        tensors[p + "mlp.down_proj.weight"] = w((N_EMBD, N_FF))

    save_safetensors(out_dir / "model.safetensors", tensors)


def main() -> None:
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "models-test" / "tiny-hf-llama"
    out_dir.mkdir(parents=True, exist_ok=True)

    write_config(out_dir)
    write_tokenizer(out_dir)
    write_weights(out_dir)

    print(f"wrote tiny HF model to {out_dir}")


if __name__ == "__main__":
    main()
