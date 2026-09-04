// HTTP layer: OpenAI-compatible endpoints plus llama-server-style utility
// endpoints (/health, /props, /tokenize, /detokenize, /apply-template).
#pragma once

#include "args.h"
#include "engine.h"

// Blocks serving requests until the process is signalled. Returns the process
// exit code.
int onyx_server_run(onyx_engine & engine, const onyx_args & args);
