#!/usr/bin/env python3
"""Minimal safetensors -> GGUF converter for jx-engine.

This is NOT a vendored/wrapped copy of llama.cpp's convert_hf_to_gguf.py.
It is a small, self-contained converter that depends on nothing beyond the
Python standard library plus numpy - no torch, no transformers, no
sentencepiece, no `safetensors` or `gguf` packages - so that jx-engine's
first-load conversion path never has to install a heavyweight ML stack.

Scope, deliberately narrow: standard Hugging Face "LlamaForCausalLM"
directories - config.json + one or more *.safetensors weight files (plain or
sharded via model.safetensors.index.json) + a tokenizer.json byte-level BPE
vocabulary. Anything else (a different architecture, a SentencePiece
tokenizer.model, quantized/exotic dtypes, etc.) is refused with a message
naming the specific limitation, rather than attempting a best-effort and
possibly-wrong conversion.

Tensor name mapping, GGUF metadata keys, and the on-disk GGUF layout (value
types, array encoding, dimension order, alignment) were cross-checked against
llama.cpp's gguf-py writer and src/llama-vocab.cpp / src/llama-model.cpp at
tag b10711, but this script does not import or execute any of that code.

Usage:
    python3 convert-safetensors.py <hf_model_dir> --outfile out.gguf [--outtype f16|f32]
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from pathlib import Path

import numpy as np

SUPPORTED_ARCHITECTURES = {"LlamaForCausalLM"}
SUPPORTED_MODEL_TYPES = {"llama"}

GGUF_MAGIC = 0x46554747  # "GGUF" read as a little-endian uint32
GGUF_VERSION = 3
ALIGNMENT = 32

GGML_TYPE_F32 = 0
GGML_TYPE_F16 = 1

# GGUF value type tags (gguf-py/gguf/constants.py::GGUFValueType).
T_UINT8, T_INT8, T_UINT16, T_INT16, T_UINT32, T_INT32, T_FLOAT32, T_BOOL, \
    T_STRING, T_ARRAY, T_UINT64, T_INT64, T_FLOAT64 = range(13)

# tokenizer.ggml.token_type values (gguf-py/gguf/constants.py::TokenType).
TOKTYPE_NORMAL, TOKTYPE_UNKNOWN, TOKTYPE_CONTROL = 1, 2, 3

# HF Llama tensor name -> GGUF llama-arch tensor name.
TOP_LEVEL_MAP = {
    "model.embed_tokens.weight": "token_embd.weight",
    "model.norm.weight": "output_norm.weight",
    "lm_head.weight": "output.weight",
}
LAYER_SUFFIX_MAP = {
    "input_layernorm.weight": "attn_norm.weight",
    "self_attn.q_proj.weight": "attn_q.weight",
    "self_attn.k_proj.weight": "attn_k.weight",
    "self_attn.v_proj.weight": "attn_v.weight",
    "self_attn.o_proj.weight": "attn_output.weight",
    "post_attention_layernorm.weight": "ffn_norm.weight",
    "mlp.gate_proj.weight": "ffn_gate.weight",
    "mlp.up_proj.weight": "ffn_up.weight",
    "mlp.down_proj.weight": "ffn_down.weight",
}
LAYER_RE = re.compile(r"^model\.layers\.(\d+)\.(.+)$")

REQUIRED_LAYER_SUFFIXES = list(LAYER_SUFFIX_MAP.values())


class ConvertError(RuntimeError):
    """A clearly-scoped conversion failure (unsupported input), as opposed to
    an unexpected bug. Caught in main() and printed without a traceback."""


# --------------------------------------------------------------------------
# safetensors reading (hand-rolled: 8-byte LE header length, JSON header,
# raw little-endian tensor bytes - https://github.com/huggingface/safetensors)
# --------------------------------------------------------------------------

_ST_DTYPES = {
    "F32": np.dtype("<f4"),
    "F16": np.dtype("<f2"),
    "I64": np.dtype("<i8"),
    "I32": np.dtype("<i4"),
    "I16": np.dtype("<i2"),
    "I8": np.dtype("<i1"),
    "U8": np.dtype("<u1"),
    "BOOL": np.dtype("<?"),
}


def _bf16_to_f32(raw: np.ndarray) -> np.ndarray:
    # bf16 is the top 16 bits of an f32; numpy has no native bf16, so widen
    # by shifting into the high half of a uint32 and reinterpreting.
    as_u16 = raw.view(np.uint16).astype(np.uint32)
    return (as_u16 << 16).view(np.float32)


def load_safetensors_file(path: Path) -> dict[str, np.ndarray]:
    with open(path, "rb") as f:
        header_len = struct.unpack("<Q", f.read(8))[0]
        header = json.loads(f.read(header_len))
        data = np.fromfile(f, dtype=np.uint8)

    tensors: dict[str, np.ndarray] = {}
    for name, meta in header.items():
        if name == "__metadata__":
            continue
        dtype_str = meta["dtype"]
        shape = meta["shape"]
        start, end = meta["data_offsets"]
        raw = data[start:end]
        if dtype_str == "BF16":
            arr = _bf16_to_f32(raw)
        elif dtype_str in _ST_DTYPES:
            arr = raw.view(_ST_DTYPES[dtype_str])
        else:
            raise ConvertError(
                f"tensor '{name}' has unsupported safetensors dtype '{dtype_str}' "
                "(this converter handles F32, F16, BF16 and integer/bool tensors only)"
            )
        tensors[name] = arr.reshape(shape) if shape else arr
    return tensors


def load_all_tensors(model_dir: Path) -> dict[str, np.ndarray]:
    index_path = model_dir / "model.safetensors.index.json"
    if index_path.is_file():
        index = json.loads(index_path.read_text())
        weight_map = index.get("weight_map")
        if not isinstance(weight_map, dict) or not weight_map:
            raise ConvertError(f"'{index_path.name}' has no usable 'weight_map'")
        by_file: dict[str, list[str]] = {}
        for tname, fname in weight_map.items():
            by_file.setdefault(fname, []).append(tname)
        tensors: dict[str, np.ndarray] = {}
        for fname, names in by_file.items():
            fpath = model_dir / fname
            if not fpath.is_file():
                raise ConvertError(f"shard '{fname}' listed in {index_path.name} is missing")
            shard = load_safetensors_file(fpath)
            for n in names:
                if n not in shard:
                    raise ConvertError(f"tensor '{n}' missing from shard '{fname}'")
                tensors[n] = shard[n]
        return tensors

    files = sorted(model_dir.glob("*.safetensors"))
    if not files:
        raise ConvertError(f"no *.safetensors files found in '{model_dir}'")
    if len(files) > 1:
        raise ConvertError(
            f"found {len(files)} *.safetensors files in '{model_dir}' but no "
            "model.safetensors.index.json - sharded models must ship an index file"
        )
    return load_safetensors_file(files[0])


# --------------------------------------------------------------------------
# GGUF writing (hand-rolled: magic/version/counts header, then KV metadata,
# then tensor-info records, then 32-byte-aligned raw tensor data)
# --------------------------------------------------------------------------

def _pack_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<Q", len(b)) + b


def _encode_scalar(vtype: int, value) -> bytes:
    if vtype == T_STRING:
        return _pack_str(value)
    if vtype == T_UINT32:
        return struct.pack("<I", value)
    if vtype == T_INT32:
        return struct.pack("<i", value)
    if vtype == T_UINT64:
        return struct.pack("<Q", value)
    if vtype == T_FLOAT32:
        return struct.pack("<f", value)
    if vtype == T_BOOL:
        return struct.pack("<?", value)
    if vtype == T_ARRAY:
        elem_type, items = value
        out = struct.pack("<I", elem_type) + struct.pack("<Q", len(items))
        for item in items:
            out += _encode_scalar(elem_type, item)
        return out
    raise ValueError(f"unhandled GGUF value type {vtype}")


def _pad(n: int, align: int = ALIGNMENT) -> int:
    return ((n + align - 1) // align) * align


class GGUFWriter:
    def __init__(self):
        self.kv: list[tuple[str, int, object]] = []
        self.tensors: list[tuple[str, np.ndarray]] = []

    def add(self, key: str, vtype: int, value) -> None:
        self.kv.append((key, vtype, value))

    def add_string(self, key: str, val: str) -> None:
        self.add(key, T_STRING, val)

    def add_u32(self, key: str, val: int) -> None:
        self.add(key, T_UINT32, val)

    def add_i32(self, key: str, val: int) -> None:
        self.add(key, T_INT32, val)

    def add_f32(self, key: str, val: float) -> None:
        self.add(key, T_FLOAT32, val)

    def add_bool(self, key: str, val: bool) -> None:
        self.add(key, T_BOOL, val)

    def add_array_str(self, key: str, vals) -> None:
        self.add(key, T_ARRAY, (T_STRING, list(vals)))

    def add_array_i32(self, key: str, vals) -> None:
        self.add(key, T_ARRAY, (T_INT32, list(vals)))

    def add_tensor(self, name: str, arr: np.ndarray) -> None:
        self.tensors.append((name, arr))

    def write(self, path: Path) -> None:
        infos = []
        offset = 0
        for name, arr in self.tensors:
            dims = list(arr.shape)[::-1] or [1]
            dtype = GGML_TYPE_F32 if arr.dtype == np.float32 else GGML_TYPE_F16
            infos.append((name, dims, dtype, arr, offset))
            offset += _pad(arr.nbytes)

        with open(path, "wb") as f:
            f.write(struct.pack("<I", GGUF_MAGIC))
            f.write(struct.pack("<I", GGUF_VERSION))
            f.write(struct.pack("<Q", len(self.tensors)))
            f.write(struct.pack("<Q", len(self.kv)))

            for key, vtype, value in self.kv:
                f.write(_pack_str(key))
                f.write(struct.pack("<I", vtype))
                f.write(_encode_scalar(vtype, value))

            for name, dims, dtype, _arr, off in infos:
                f.write(_pack_str(name))
                f.write(struct.pack("<I", len(dims)))
                for d in dims:
                    f.write(struct.pack("<Q", d))
                f.write(struct.pack("<I", dtype))
                f.write(struct.pack("<Q", off))

            pos = f.tell()
            pad = _pad(pos) - pos
            if pad:
                f.write(b"\0" * pad)

            for _name, _dims, _dtype, arr, _off in infos:
                raw = np.ascontiguousarray(arr).tobytes()
                f.write(raw)
                pad = _pad(len(raw)) - len(raw)
                if pad:
                    f.write(b"\0" * pad)


# --------------------------------------------------------------------------
# HF -> GGUF mapping
# --------------------------------------------------------------------------

def load_config(model_dir: Path) -> dict:
    config_path = model_dir / "config.json"
    if not config_path.is_file():
        raise ConvertError(f"'{config_path}' not found - not a Hugging Face model directory")
    config = json.loads(config_path.read_text())

    archs = config.get("architectures") or []
    model_type = config.get("model_type")
    if not (set(archs) & SUPPORTED_ARCHITECTURES) and model_type not in SUPPORTED_MODEL_TYPES:
        raise ConvertError(
            "unsupported model: this converter only handles the standard HF Llama "
            f"architecture (architectures={archs!r}, model_type={model_type!r}); "
            "convert with llama.cpp's convert_hf_to_gguf.py instead"
        )
    return config


def load_tokenizer(model_dir: Path) -> dict:
    tok_path = model_dir / "tokenizer.json"
    if not tok_path.is_file():
        raise ConvertError(
            f"'{tok_path}' not found - this converter only supports a byte-level "
            "BPE tokenizer.json (e.g. as written by scripts/make-tiny-hf-model.py), "
            "not a SentencePiece tokenizer.model"
        )
    tok = json.loads(tok_path.read_text())
    model = tok.get("model") or {}
    if model.get("type") != "BPE":
        raise ConvertError(
            f"unsupported tokenizer.json model type {model.get('type')!r}; "
            "only a byte-level BPE tokenizer is supported"
        )
    vocab = model.get("vocab")
    if not isinstance(vocab, dict) or not vocab:
        raise ConvertError("tokenizer.json has no usable 'model.vocab'")
    return tok


def build_vocab(tok: dict) -> tuple[list[str], list[int], list[str]]:
    vocab: dict[str, int] = tok["model"]["vocab"]
    merges = tok["model"].get("merges") or []
    max_id = max(vocab.values())
    tokens = ["" for _ in range(max_id + 1)]
    for text, tid in vocab.items():
        tokens[tid] = text
    if any(t == "" for t in tokens):
        raise ConvertError("tokenizer.json vocab has gaps in token ids")

    special_ids = {}
    for entry in tok.get("added_tokens") or []:
        if entry.get("special"):
            special_ids[entry["content"]] = entry["id"]

    toktypes = [TOKTYPE_NORMAL] * len(tokens)
    for content, tid in special_ids.items():
        toktypes[tid] = TOKTYPE_UNKNOWN if content == "<unk>" else TOKTYPE_CONTROL

    return tokens, toktypes, list(merges)


def resolve_special_token_id(config: dict, key: str) -> int | None:
    val = config.get(key)
    if val is None:
        return None
    if isinstance(val, list):
        return int(val[0]) if val else None
    return int(val)


def gguf_tensor_name(hf_name: str) -> str | None:
    if hf_name in TOP_LEVEL_MAP:
        return TOP_LEVEL_MAP[hf_name]
    m = LAYER_RE.match(hf_name)
    if m:
        idx, rest = m.group(1), m.group(2)
        if rest in LAYER_SUFFIX_MAP:
            return f"blk.{idx}.{LAYER_SUFFIX_MAP[rest]}"
    return None


def convert(model_dir: Path, outfile: Path, outtype: str) -> None:
    config = load_config(model_dir)
    tok = load_tokenizer(model_dir)

    n_embd = config["hidden_size"]
    n_ff = config["intermediate_size"]
    n_layer = config["num_hidden_layers"]
    n_head = config["num_attention_heads"]
    n_head_kv = config.get("num_key_value_heads", n_head)
    n_ctx = config.get("max_position_embeddings", 2048)
    rms_eps = config.get("rms_norm_eps", 1e-5)
    rope_theta = config.get("rope_theta", 10000.0)
    vocab_size = config.get("vocab_size")
    tie_embeddings = bool(config.get("tie_word_embeddings", False))

    if n_embd % n_head != 0:
        raise ConvertError(f"hidden_size ({n_embd}) is not divisible by num_attention_heads ({n_head})")
    rope_dim = n_embd // n_head

    raw_tensors = load_all_tensors(model_dir)

    mapped: dict[str, np.ndarray] = {}
    for hf_name, arr in raw_tensors.items():
        if hf_name.endswith(".self_attn.rotary_emb.inv_freq"):
            continue  # a derived buffer, not a learned weight - GGUF recomputes it
        gname = gguf_tensor_name(hf_name)
        if gname is None:
            raise ConvertError(f"unsupported tensor '{hf_name}' - this converter only handles a plain Llama block")
        mapped[gname] = arr

    if tie_embeddings and "output.weight" not in mapped and "token_embd.weight" in mapped:
        mapped["output.weight"] = mapped["token_embd.weight"]

    required = ["token_embd.weight", "output_norm.weight", "output.weight"]
    for i in range(n_layer):
        required += [f"blk.{i}.{suffix}" for suffix in REQUIRED_LAYER_SUFFIXES]
    missing = [name for name in required if name not in mapped]
    if missing:
        raise ConvertError(f"model is missing required tensor(s): {', '.join(missing)}")

    tokens, toktypes, merges = build_vocab(tok)
    if vocab_size is not None and len(tokens) != vocab_size:
        raise ConvertError(
            f"config.json vocab_size ({vocab_size}) does not match tokenizer.json vocab size ({len(tokens)})"
        )

    bos_id = resolve_special_token_id(config, "bos_token_id")
    eos_id = resolve_special_token_id(config, "eos_token_id")

    w = GGUFWriter()
    w.add_string("general.architecture", "llama")
    w.add_string("general.name", config.get("_name_or_path") or model_dir.name)
    w.add_u32("general.file_type", 1 if outtype == "f16" else 0)

    w.add_u32("llama.context_length", n_ctx)
    w.add_u32("llama.embedding_length", n_embd)
    w.add_u32("llama.block_count", n_layer)
    w.add_u32("llama.feed_forward_length", n_ff)
    w.add_u32("llama.attention.head_count", n_head)
    w.add_u32("llama.attention.head_count_kv", n_head_kv)
    w.add_f32("llama.attention.layer_norm_rms_epsilon", float(rms_eps))
    w.add_u32("llama.rope.dimension_count", rope_dim)
    w.add_f32("llama.rope.freq_base", float(rope_theta))
    w.add_u32("llama.vocab_size", len(tokens))

    w.add_string("tokenizer.ggml.model", "gpt2")
    w.add_string("tokenizer.ggml.pre", "default")
    w.add_array_str("tokenizer.ggml.tokens", tokens)
    w.add_array_i32("tokenizer.ggml.token_type", toktypes)
    w.add("tokenizer.ggml.merges", T_ARRAY, (T_STRING, merges))
    if bos_id is not None:
        w.add_u32("tokenizer.ggml.bos_token_id", bos_id)
        w.add_bool("tokenizer.ggml.add_bos_token", True)
    if eos_id is not None:
        w.add_u32("tokenizer.ggml.eos_token_id", eos_id)
        w.add_bool("tokenizer.ggml.add_eos_token", False)

    to_f16 = outtype == "f16"
    for name in ["token_embd.weight", "output.weight"] + [
        f"blk.{i}.{suffix}" for i in range(n_layer)
        for suffix in ["attn_q.weight", "attn_k.weight", "attn_v.weight", "attn_output.weight",
                       "ffn_gate.weight", "ffn_up.weight", "ffn_down.weight"]
    ]:
        arr = mapped[name].astype(np.float32)
        w.add_tensor(name, arr.astype(np.float16) if to_f16 else arr)

    for name in ["output_norm.weight"] + [
        f"blk.{i}.{suffix}" for i in range(n_layer)
        for suffix in ["attn_norm.weight", "ffn_norm.weight"]
    ]:
        # norm weights always stay F32, matching llama.cpp's own convention
        w.add_tensor(name, mapped[name].astype(np.float32))

    outfile.parent.mkdir(parents=True, exist_ok=True)
    w.write(outfile)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model", type=Path, help="HF model directory (config.json + *.safetensors + tokenizer.json)")
    ap.add_argument("--outfile", type=Path, required=True, help="path to write the GGUF file to")
    ap.add_argument("--outtype", choices=["f16", "f32"], default="f16", help="output tensor precision (default: f16)")
    args = ap.parse_args()

    if not args.model.is_dir():
        print(f"error: '{args.model}' is not a directory", file=sys.stderr)
        return 1

    try:
        convert(args.model, args.outfile, args.outtype)
    except ConvertError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(f"wrote {args.outfile}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
