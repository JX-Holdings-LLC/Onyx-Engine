#!/usr/bin/env python3
"""Small format and input-safety checks for the standalone converter."""
import json
import runpy
import struct
import tempfile
from pathlib import Path

import numpy as np

converter = runpy.run_path(str(Path(__file__).with_name("convert-safetensors.py")))

tokens, kinds, merges = converter["build_vocab"]({
    "model": {"vocab": {"a": 0}, "merges": [["a", "b"]]},
    "added_tokens": [{"id": 3, "content": "<think>", "special": True}],
}, 4)
assert tokens == ["a", "[PAD1]", "[PAD2]", "<think>"]
assert kinds[3] == converter["TOKTYPE_CONTROL"]
assert merges == ["a b"]

with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    (root / "model.safetensors.index.json").write_text(json.dumps({
        "weight_map": {"x": "../outside.safetensors"}
    }))
    try:
        converter["load_all_tensors"](root)
        raise AssertionError("index traversal was accepted")
    except converter["ConvertError"]:
        pass

    source = np.arange(16, dtype=np.float32).reshape(4, 4)
    writer = converter["GGUFWriter"]()
    writer.add_tensor("q", source, "f32", heads=1)
    output = root / "q.gguf"
    writer.write(output)
    data = output.read_bytes()
    assert data[:4] == b"GGUF"
    position = 24  # magic, version, tensor count, metadata count
    length = struct.unpack_from("<Q", data, position)[0]
    position += 8 + length
    rank = struct.unpack_from("<I", data, position)[0]
    position += 4 + rank * 8 + 4 + 8  # dims, type, tensor offset
    position = (position + 31) // 32 * 32
    actual = np.frombuffer(data, dtype="<f4", count=16, offset=position).reshape(4, 4)
    expected = source.reshape(1, 2, 2, 4).swapaxes(1, 2).reshape(4, 4)
    np.testing.assert_array_equal(actual, expected)

print("converter regression checks passed")
