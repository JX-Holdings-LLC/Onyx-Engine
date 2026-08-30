// HTTP layer: OpenAI-compatible endpoints plus llama-server-style utility
// endpoints (/health, /props, /tokenize, /detokenize, /apply-template).
#pragma once

#include "args.h"
#include "engine.h"

// Blocks serving requests until the process is signalled. Returns the process
// exit code.
int jx_server_run(jx_engine & engine, const jx_args & args);
