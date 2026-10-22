// HTTP layer: OpenAI-compatible endpoints plus llama-server-style utility
// endpoints (/health, /props, /metrics, /tokenize, /detokenize,
// /apply-template).
#pragma once

#include "args.h"
#include "engine.h"

#include <functional>

// Binds --host/--port, starts answering at once, then runs `load` on the
// calling thread. Until `load` returns true, /health and every other route
// answer 503 "Loading model" (llama-server's shape), so a supervisor can tell
// a loading engine from a dead one. If `load` returns false the server stops
// and this returns 1; otherwise it serves until the process is signalled.
// Returns the process exit code.
int onyx_server_run(onyx_engine & engine, const onyx_args & args, const std::function<bool()> & load);
