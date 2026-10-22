#!/usr/bin/env python3
"""Builds a tiny random-weight llama-architecture GGUF for smoke testing.

The tokenizer is copied from llama.cpp's checked-in test vocab
(models/ggml-vocab-llama-spm.gguf), so the result tokenizes real text but
generates gibberish. It exists so onyx-engine's endpoints can be exercised
offline in CI without downloading a real model.

With --decision it builds a tiny openjev-type decision model instead
("llama.decision.type" plus a "systemone" chat template), for the
/v1/systemone smoke tests.

Usage: python3 scripts/make-tiny-model.py [--decision] [output.gguf]
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# same resolution order as CMakeLists.txt: npm package first, then a manually
# placed vendor tree
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

VOCAB_SRC = LLAMA_SRC / "models" / "ggml-vocab-llama-spm.gguf"

N_VOCAB = 32000
N_EMBD  = 64
N_HEAD  = 4
N_HEAD_KV = 4
N_LAYER = 2
N_FF    = 128
N_CTX   = 512

# The role markers are plain text (this vocab has no chat special tokens) and
# the generation prompt is empty on purpose: a non-empty generation prompt made
# of non-special tokens does not survive SPM retokenization, which breaks
# grammar prefill. Real chat models use dedicated special tokens and do not
# have this problem.
CHAT_TEMPLATE = (
    "{% for message in messages %}"
    "{{ message['role'] + ': ' + message['content'] + '\n' }}"
    "{% endfor %}"
)


# openjev layout: the options are lettered A, B, ... and the answer is read
# from the logits of those letters at the last prompt token
SYSTEMONE_TEMPLATE = (
    "{% set letters = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz' %}"
    "State: {{ state if state is string else state | tojson }}\n"
    "Question: {{ instructions if instructions is string else instructions | tojson }}\n"
    "{% for o in options %}[{{ letters[loop.index0] }}] {{ o.key }}"
    "{% if o.description %}: {{ o.description if o.description is string else o.description | tojson }}{% endif %}\n"
    "{% endfor %}Answer:"
)


def main() -> None:
    args = [a for a in sys.argv[1:] if a != "--decision"]
    decision = len(args) != len(sys.argv) - 1
    default_name = "tiny-llama-decision.gguf" if decision else "tiny-llama-random.gguf"
    out_path = Path(args[0]) if args else REPO / "models-test" / default_name
    out_path.parent.mkdir(parents=True, exist_ok=True)

    reader = gguf.GGUFReader(str(VOCAB_SRC))

    def field_str_list(name: str) -> list:
        f = reader.fields[name]
        return [bytes(f.parts[i]).decode("utf-8", errors="replace") for i in f.data]

    def field_arr(name: str, dtype) -> np.ndarray:
        f = reader.fields[name]
        return np.array([f.parts[i][0] for i in f.data], dtype=dtype)

    tokens = field_str_list("tokenizer.ggml.tokens")[:N_VOCAB]
    scores = field_arr("tokenizer.ggml.scores", np.float32)[:N_VOCAB].tolist()
    toktypes = field_arr("tokenizer.ggml.token_type", np.int32)[:N_VOCAB].tolist()

    w = gguf.GGUFWriter(str(out_path), "llama")
    w.add_name("onyx-engine-tiny-test")
    w.add_context_length(N_CTX)
    w.add_embedding_length(N_EMBD)
    w.add_block_count(N_LAYER)
    w.add_feed_forward_length(N_FF)
    w.add_head_count(N_HEAD)
    w.add_head_count_kv(N_HEAD_KV)
    w.add_layer_norm_rms_eps(1e-5)
    w.add_vocab_size(N_VOCAB)
    w.add_rope_dimension_count(N_EMBD // N_HEAD)
    w.add_file_type(gguf.LlamaFileType.ALL_F32)

    w.add_tokenizer_model("llama")
    w.add_tokenizer_pre("default")
    w.add_token_list(tokens)
    w.add_token_scores(scores)
    w.add_token_types(toktypes)
    w.add_bos_token_id(1)
    w.add_eos_token_id(2)
    w.add_unk_token_id(0)
    w.add_add_bos_token(True)
    w.add_add_eos_token(False)
    if decision:
        w.add_chat_template([
            {"name": "default", "template": CHAT_TEMPLATE},
            {"name": "systemone", "template": SYSTEMONE_TEMPLATE},
        ])
        w.add_string("llama.decision.type", "openjev")
        w.add_float32("llama.decision.temperature.choice", 0.85)
        w.add_float32("llama.decision.temperature.noul", 1.5)
    else:
        w.add_chat_template(CHAT_TEMPLATE)

    rng = np.random.default_rng(42)

    def t(name: str, shape: tuple) -> None:
        w.add_tensor(name, rng.standard_normal(size=shape, dtype=np.float32) * 0.02)

    head_dim = N_EMBD // N_HEAD
    t("token_embd.weight", (N_VOCAB, N_EMBD))
    t("output_norm.weight", (N_EMBD,))
    t("output.weight", (N_VOCAB, N_EMBD))
    for i in range(N_LAYER):
        p = f"blk.{i}."
        t(p + "attn_norm.weight", (N_EMBD,))
        t(p + "attn_q.weight", (N_EMBD, N_EMBD))
        t(p + "attn_k.weight", (N_HEAD_KV * head_dim, N_EMBD))
        t(p + "attn_v.weight", (N_HEAD_KV * head_dim, N_EMBD))
        t(p + "attn_output.weight", (N_EMBD, N_EMBD))
        t(p + "ffn_norm.weight", (N_EMBD,))
        t(p + "ffn_gate.weight", (N_FF, N_EMBD))
        t(p + "ffn_up.weight", (N_FF, N_EMBD))
        t(p + "ffn_down.weight", (N_EMBD, N_FF))

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    print(f"wrote {out_path} ({out_path.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
