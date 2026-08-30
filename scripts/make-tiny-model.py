#!/usr/bin/env python3
"""Builds a tiny random-weight llama-architecture GGUF for smoke testing.

The tokenizer is copied from llama.cpp's checked-in test vocab
(models/ggml-vocab-llama-spm.gguf), so the result tokenizes real text but
generates gibberish. It exists so jx-engine's endpoints can be exercised
offline in CI without downloading a real model.

Usage: python3 scripts/make-tiny-model.py [output.gguf]
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "vendor" / "llama.cpp" / "gguf-py"))

import numpy as np  # noqa: E402
import gguf  # noqa: E402

VOCAB_SRC = REPO / "vendor" / "llama.cpp" / "models" / "ggml-vocab-llama-spm.gguf"

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


def main() -> None:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "models-test" / "tiny-llama-random.gguf"
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
    w.add_name("jx-engine-tiny-test")
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
