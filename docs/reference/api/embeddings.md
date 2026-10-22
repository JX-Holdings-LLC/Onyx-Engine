[Documentation](../../README.md) › [Reference](../README.md) › [API reference](README.md) › Embeddings

# Embeddings

`POST /v1/embeddings`, available only when the process was started with `--embedding`. See [embeddings and reranking](../../guides/embeddings-and-reranking.md) for how to launch and use it.

Returns `501` unless the process was started with `--embedding`. Requires
`input`; missing it is `400`. `dimensions` and any `encoding_format` other than `float` are refused with `400`.

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
