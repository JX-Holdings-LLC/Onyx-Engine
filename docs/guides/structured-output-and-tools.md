[Documentation](../README.md) › [Guides](README.md) › Structured output and tools

# Structured output and tools

Three ways to constrain what a model writes, in rising order of control: a JSON schema, a GBNF grammar, and tool definitions. All three work by constraining the sampler, so the output is valid by construction instead of by hope.

## A JSON schema

Send the schema in `response_format`, in the OpenAI shape:

```bash
curl http://127.0.0.1:8080/v1/chat/completions -H 'Content-Type: application/json' -d '{
  "messages": [{"role": "user", "content": "Extract: Ada Lovelace, born 1815."}],
  "response_format": {
    "type": "json_schema",
    "json_schema": {"schema": {
      "type": "object",
      "properties": {"name": {"type": "string"}, "born": {"type": "integer"}},
      "required": ["name", "born"]
    }}
  }
}'
```

The accepted forms of `response_format` are `{"type":"json_object"}` (optionally with a `schema`; without one the output is held to a JSON object), `{"type":"json_schema","json_schema":{"schema":...}}` and `{"type":"text"}`; any other `type` is a `400`. A bare `json_schema` object at the top level of the request also works, and `/v1/completions` accepts `json_schema` the same way. The schema is converted to a grammar with llama.cpp's `json_schema_to_grammar`.

Things to know:

- **A schema constrains the shape, not the truth.** A model can still write a wrong `born` year, as a valid integer.
- **A length cut is an error, not half an object.** If generation stops at `max_tokens` or the slot's context before the JSON is complete, the request fails with `400` (`invalid_request_error`) and a message naming which limit was hit, instead of a `200` carrying invalid JSON. A streaming request has already sent the partial deltas; its stream ends with that error as the last `data:` frame and no `[DONE]`. Leave generous room in `max_tokens`. (Before 0.7.0 the response was a `200` with `finish_reason: "length"` and the partial text.)
- **A schema llama.cpp cannot convert** comes back as a `400` with the reason.
- **Song Deck's schemas are a regression check.** `tests/schemas/song-deck/` holds two of Song Deck's schemas in the dialects it sends to local servers (OpenAI strict: every property required, optional ones as nullable unions; and plain JSON schema with numeric and array constraints kept). The smoke test runs both through `response_format` and checks that the output opens as the schema's object.

## A grammar

For anything that is not JSON (a fixed vocabulary, a date format, a DSL), send a GBNF grammar as a string:

```bash
curl http://127.0.0.1:8080/v1/completions -H 'Content-Type: application/json' -d '{
  "prompt": "Is the sky blue? Answer: ",
  "grammar": "root ::= \"yes\" | \"no\"",
  "max_tokens": 4
}'
```

A malformed grammar is a `400`. The `grammar` field works on both chat and text completions. Because it constrains sampling, it also changes what `logprobs` shows: the reported probabilities are the model's raw distribution, so a token the grammar forced can legitimately show a very low logprob.

## Tool calls

Declare tools in the OpenAI format. The engine renders them through the model's own chat template and parses the model's output back into structured `tool_calls`:

```bash
curl http://127.0.0.1:8080/v1/chat/completions -H 'Content-Type: application/json' -d '{
  "messages": [{"role": "user", "content": "What is the weather in Oslo?"}],
  "tools": [{"type": "function", "function": {
    "name": "get_weather",
    "description": "Current weather for a city",
    "parameters": {"type": "object", "properties": {"city": {"type": "string"}}, "required": ["city"]}
  }}],
  "tool_choice": "auto"
}'
```

`tool_choice` is `auto`, `none`, `required`, or a function object naming one provided tool. `parallel_tool_calls` is accepted. When the model calls a tool the response has `finish_reason: "tool_calls"` and a `message.tool_calls` array; when streaming, the call arrives as incremental `tool_calls` deltas.

How well this works depends on the model and its template. The engine supplies whatever tool-call grammar the template asks for and parses with the parser the template selected; a model that was never trained for tools will not become reliable because a grammar is attached. If a template renders tools badly, inspect the prompt with `POST /apply-template` and, if you must, override it with `--chat-template-file`.

## Combining them

`grammar`, `json_schema`, `response_format` and `tools` are not independent switches. The chat template decides which grammar type applies: a caller's `grammar` wins as a user grammar, `json_schema` or `response_format` become an output-format grammar, and otherwise the template's own tool-call grammar applies. Send one constraint per request.

## Reasoning and constraints

A request that asks for a reasoning budget while a non-lazy grammar is active is refused with a `400`, since the engine cannot force the closing tag through a grammar that insists on a specific next token. Use a schema for the answer and let the model think freely, or constrain and skip the budget.

Reference: [chat completions](../reference/api/chat-completions.md), [text completions](../reference/api/completions.md). How the grammar is built and applied: [chat templates, grammar and sampling](../architecture/chat-templates-and-sampling.md).
