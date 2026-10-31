// Request-body parsing helpers shared by the generation and embedding
// handlers: how request fields become generation parameters. Pure functions
// on onyx_json, unit-tested in tests/unit/test_request.cpp.
#pragma once

#include "onyx_json.h"
#include "sampler.h"

#include <cstdint>
#include <string>
#include <vector>

// "stop": a string or an array of strings; empty strings are dropped.
std::vector<std::string> onyx_parse_stop(const onyx_json & body);

// max_completion_tokens, then max_tokens, then n_predict: the first present,
// non-null one; -1 (until EOG or the context limit) when none is.
int32_t onyx_parse_max_tokens(const onyx_json & body);

// temperature, top_p, top_k, min_p, seed, repeat_penalty, repeat_last_n,
// presence_penalty and frequency_penalty over the sampler's defaults;
// `default_seed` (--seed) unless the body sets one.
onyx_sampling_params onyx_parse_sampling(const onyx_json & body, uint32_t default_seed);

// Embeddings "dimensions" (OpenAI's Matryoshka truncation): keeps the first
// `dims` values and L2-normalises what is left. A vector already that short
// is only renormalised.
void onyx_truncate_embedding(std::vector<float> & v, size_t dims);
