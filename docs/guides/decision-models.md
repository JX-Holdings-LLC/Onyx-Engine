[Documentation](../README.md) › [Guides](README.md) › Decision models

# Decision models

A decision model (a TypeSafe-style "System One" model) does not write text. You hand it a state and some typed questions, and it answers each from a single prefill. The engine serves them on `POST /v1/systemone`, following llama-server's request and response shapes.

![A decision request is rendered per question, prefilled once, read from family-specific outputs and softmaxed into answers](../../assets/diagrams/svg/decision-models.svg)

## Start one

A GGUF is a decision model when its metadata carries `<arch>.decision.type`. Nothing else is needed on the command line:

```bash
onyx-engine -m ./models/decider.gguf --alias decider --port 8083
curl http://127.0.0.1:8083/props     # ..., "decision_type": "openjev", ...
```

Such a process serves only `/v1/systemone` plus the probes and utilities. Chat and text completions and embeddings answer `501`, and `/v1/systemone` on any other kind of process answers `501`. Requests run one at a time. `--embedding` with a decision model is a load error, and a model whose family is unknown, or which lacks a `systemone` chat template, fails to load.

## Ask

```bash
curl http://127.0.0.1:8083/v1/systemone -H 'Content-Type: application/json' -d '{
  "state": "Customer message: I was charged twice and nobody has replied.",
  "questions": {
    "route":   {"type": "choice", "instructions": "Which team should handle this?",
                "criteria": {"billing": "payments and refunds", "shipping": null, "technical": null}},
    "urgency": {"type": "score",  "instructions": "How urgent is this?",
                "criteria": ["can wait", "this week", "today", "right now"]},
    "angry":   {"type": "noul",   "instructions": "Is the customer angry?"}
  }
}'
```

| Type | You supply | You get |
|---|---|---|
| `choice` | options, each with a description or `null` | the pick, a probability per option, a confidence |
| `score` | 2 to 10 level descriptions, lowest first | a fractional score, a legend, probabilities, a confidence |
| `noul` | optionally, descriptions of `true` and `false` | a number between 0 and 1 |

`answers` keeps the order of `questions`. `confidence` is 0 when every option is equally likely. The full request and response shapes are in [the reference](../reference/api/systemone.md).

## Images

With a projector (`--mmproj`) the families whose template has a place for images accept `images` (data URLs, at most 8), or `image_url` parts inside a `state` made of chat messages: `openjev`, `pplx-decider`, `lfm2-d1`, `h2o-lightning`, `vllm-d3`, `lfm2-d1-omni` and `clef`. `lfm2-d1-omni` also takes one audio clip instead of images; its media answers are not calibrated (temperature 1). The other families answer image requests with `501`. A prompt with media is never reused by the next request.

## Speed

Questions in one request that share a prompt prefix (the causal families) evaluate it once, and a later request that begins the same way reuses it. Put the long, stable part of your prompt in the state and the short part in the question, and keep `--cache-reuse` at its default. `--cache-reuse 0` turns this off.

## What to expect when it fails

| Status | Meaning |
|---|---|
| `400` | malformed request, too many options for the family (52 for openjev, 255 for most), a question whose options (and, for clef, lfm2-d1-omni and vllm-d3, the media they read) do not fit one `--batch-size` batch, or media that cannot be decoded |
| `501` | not a decision model; or a family that cannot take images; or a build that cannot take them |

Mechanism, family by family: [decision models architecture](../architecture/decision-models.md).
