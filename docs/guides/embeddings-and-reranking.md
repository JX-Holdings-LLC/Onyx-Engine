[Documentation](../README.md) › [Guides](README.md) › Embeddings and reranking

# Embeddings and reranking

An embedding process is a different kind of process, not a mode you toggle per request. Start it with `--embedding`, and it serves `/v1/embeddings` and the utilities and nothing that generates.

## Start an embedding server

```bash
onyx-engine -m ./models/embed-model.gguf --embedding --alias embed --port 8082
```

`--embedding` has side effects worth knowing:

- one slot: `-np` is forced to 1 and there is no batching loop;
- context shift is disabled;
- the physical batch size is set equal to the logical one, so each input has to fit `-b` (default 2048 tokens) and the context window;
- `/v1/chat/completions` and `/v1/completions` answer `501`.

`--pooling` picks how token vectors become one vector: `none`, `mean`, `cls`, `last` or `rank`. Leave it unset to use the model's own default, which is almost always right. `--pooling` without `--embedding` is an error.

## Ask for vectors

```bash
curl http://127.0.0.1:8082/v1/embeddings -H 'Content-Type: application/json' \
  -d '{"input": ["first text", "second text"]}'
```

`input` may be a string, an array of strings, an array of token ids (one pre-tokenized input) or an array of arrays of token ids. Each item gets `{"object":"embedding","index":i,"embedding":[...]}`, and the vectors are L2-normalized, so a dot product is a cosine similarity. `usage` counts input tokens only.

`encoding_format: "base64"`, which the official OpenAI Python and JS clients send by default, works and is decoded by the client. `dimensions` keeps the first N values of each vector and renormalises them (OpenAI's Matryoshka truncation, from 0.8.0); it only makes sense for models trained for it, such as nomic-embed or the text-embedding-3 style models, and Onyx cannot tell whether yours was. It must be an integer from 1 to the model's `n_embd`, and is refused under `--pooling rank`. An `encoding_format` other than `float` or `base64` is refused with `400`.

## Reranking

A reranking model needs `--pooling rank`:

```bash
onyx-engine -m ./models/rerank-model.gguf --embedding --pooling rank --alias rerank
```

Under rank pooling each item's `embedding` holds the model's raw classifier score or scores (one per classifier output, usually one: relevance), not an `n_embd`-length vector, and it is **not** normalized. Send the query and document as one input in whatever pair format the model expects. A purpose-built `/v1/rerank` route is on the [roadmap](../../ROADMAP.md#next-weeks); until then you call `/v1/embeddings` and sort the scores yourself.

## Running both

An embedding model and a chat model are two processes on two ports. JX Runtime does exactly this, one child per loaded model; see [the integration notes](../integration/jx-runtime.md).

Reference: [embeddings](../reference/api/embeddings.md).
