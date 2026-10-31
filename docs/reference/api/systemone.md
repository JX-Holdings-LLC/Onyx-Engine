[Documentation](../../README.md) › [Reference](../README.md) › [API reference](README.md) › Decision models: /v1/systemone

# Decision models: /v1/systemone

`POST /v1/systemone`, available only for a decision model. For a walkthrough see [decision models](../../guides/decision-models.md); for how a question becomes an answer see [the architecture note](../../architecture/decision-models.md).

Answers typed questions about a `state` with a decision ("System One")
model: a GGUF with `<arch>.decision.type` in its metadata. Follows the
[TypeSafe API](https://docs.typesafe.ai/api) as llama-server implements it
(ggml-org/llama.cpp #29818 and the model PRs after it). No token is
generated: each answer comes from one prefill. Streaming is not supported.

A process serving a decision model answers only this route: chat and text
completions return `501`, and this route returns `501` on a process serving
any other model. `GET /props` reports the family as `decision_type`.

Request:

```json
{
  "state": "Customer message: I was charged twice and nobody has replied.",
  "questions": {
    "route":   {"type": "choice", "instructions": "Which team should handle this?",
                "criteria": {"billing": "payments and refunds", "shipping": null, "technical": null}},
    "urgency": {"type": "score",  "instructions": "How urgent is this?",
                "criteria": ["can wait", "this week", "today", "right now"]},
    "angry":   {"type": "noul",   "instructions": "Is the customer angry?"}
  }
}
```

- `state`: text, an object or an array. A non-string state reaches the model
  as JSON text. lfm2-d1, lfm2-d1-omni and vllm-d3 accept `null`.
- `questions`: question id -> question. `type` is `choice`, `score` or
  `noul`; `instructions` is text, an object or an array. `criteria` depends
  on the type: `choice` maps each option to a description (or `null`);
  `score` is an array of 2 to 10 level descriptions, lowest first; `noul`
  optionally describes `true` and `false`. The option count is capped per
  family (52 for openjev, 255 for most others).
- `images` (alias `files`): data URLs, at most 8, placed before the state.
  A `state` made of chat messages (an array, or an object with `messages`)
  can also carry `image_url` parts, which are taken out of the state.
  Needs `--mmproj`; see the family table below.

Response:

```json
{
  "model": "my-decision-model",
  "answers": {
    "route":   {"type": "choice", "choice": "billing",
                "probabilities": {"billing": 0.9998, "shipping": 0.0001, "technical": 0.0001},
                "confidence": 0.9997},
    "urgency": {"type": "score", "score": 2.0858,
                "legend": {"0": "can wait", "1": "this week", "2": "today", "3": "right now"},
                "probabilities": {"0": 0.0023, "1": 0.116, "2": 0.6753, "3": 0.2064},
                "confidence": 0.673},
    "angry":   {"type": "noul", "noul": 0.6328}
  },
  "usage": {"input_tokens": 239, "output_tokens": 0}
}
```

`answers` keeps the order of `questions`. Probabilities are scaled by the
temperatures stored in the model file; `confidence` is 0 when every option
is equally likely. `usage.input_tokens` counts the prompt tokens of every
prompt evaluated, cached or not.

Families (from `<arch>.decision.type`):

| Family | Reads | Images |
|---|---|---|
| `openjev`, `lev`, `nimble`, `pplx-decider`, `granite-switch`, `lfm2-d1`, `h2o-lightning` | logits of one label token per option, at the last prompt token | openjev, pplx-decider, lfm2-d1, h2o-lightning |
| `vllm-d3` | same, non-causal: the whole prompt is one batch | yes |
| `laya`, `lfm2-d1-omni` | embeddings at one marker token per option; the whole prompt is one batch | lfm2-d1-omni: images, or one audio clip |
| `kev` | dot product of the last token's and each option's hidden state | no |
| `clef`, `vllm-decision2` | a joint head over marked spans (all of a request's questions in one prompt, for clef) | clef |

Questions of one request that share a prompt prefix (the causal families)
evaluate it once, and a later request that starts the same way reuses it,
unless `--cache-reuse 0`. Requests run one at a time.

Errors: `400` for a malformed request, too many options, a question
whose options do not fit in one `--batch-size` batch (for clef,
lfm2-d1-omni and vllm-d3 with media, counting the media rows read in that
batch), or media that cannot be decoded; `501` as above, and for images on
a model or build that cannot take them.
