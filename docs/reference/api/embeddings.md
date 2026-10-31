[Documentation](../../README.md) › [Reference](../README.md) › [API reference](README.md) › Embeddings

# Embeddings

`POST /v1/embeddings`, available only when the process was started with `--embedding`. See [embeddings and reranking](../../guides/embeddings-and-reranking.md) for how to launch and use it.

Returns `501` unless the process was started with `--embedding`. Requires
`input`; missing it is `400`. `encoding_format` is `float` (the default: an array of numbers) or `base64` (the vector's float32 bytes, little-endian, base64-encoded, as OpenAI returns them; the official OpenAI clients ask for this by default and decode it themselves). Any other `encoding_format` is refused with `400`. `dimensions` (an integer from 1 to the model's `n_embd`) keeps the first `dimensions` values of each vector and L2-normalises them again, OpenAI's semantics for Matryoshka-trained models; it is applied before the `float`/`base64` encoding. Truncation only means something for a model trained for it (for others it just discards information); Onyx does not check. Anything outside that range, a non-integer, or `dimensions` on a `--pooling rank` process (which returns scores, not embeddings) is a `400`.

`input` accepts: a string, an array of strings, an array of integer token
ids (a single pre-tokenized input), or an array of arrays of integers
(multiple pre-tokenized inputs). Mixed-type arrays (some string items, some
non-string/non-array items) return `400`.
Each input must fit within the configured `-b` batch size and context window;
invalid token IDs are rejected with `400`.

Response:

```json
{
  "object": "list",
  "model": "my-model",
  "data": [
    {"object": "embedding", "index": 0, "embedding": [0.0123, -0.045, ...]}
  ],
  "usage": {"prompt_tokens": 12, "total_tokens": 12}
}
```

Embeddings are L2-normalized, except under `--pooling rank` (rerankers):
there each item's `embedding` holds the model's raw, unnormalized
classifier score(s), one value per classifier output (`n_cls_out`; usually
1, the relevance score), instead of an `n_embd`-length vector. For a
reranker, pass the query/document pair as the input in the format the model
expects. `usage.completion_tokens` is not reported
(embeddings have none); `prompt_tokens`/`total_tokens` are the summed input
token counts across all items in the batch.
