// Safetensors -> GGUF conversion (v2): lets `-m <path>` point at an HF-style
// safetensors model directory (or a *.safetensors file inside one) instead of
// a GGUF file. The conversion itself is delegated to
// scripts/convert-safetensors.py (a small, dependency-light converter that
// needs only python3 + numpy - see that script for scope/limitations); this
// module only resolves what to run, caches the result, and reports errors.
//
// Self-contained: no dependency on engine.h/server.h, just args.h and the
// standard library (+ llama.h for nothing beyond what args.h already pulls
// in transitively, if anything).
#pragma once

#include <string>

struct onyx_args;

// Resolves args.model_path: returns the path of a GGUF to load, converting a
// safetensors model first if needed. On failure returns an empty string and
// sets `err` to a message describing why.
std::string onyx_resolve_model(const onyx_args & args, std::string & err);
