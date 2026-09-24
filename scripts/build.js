#!/usr/bin/env node
// `npm run build`: configure + build onyx-engine with a bounded job count.
//
// A bare `cmake --build ... -j` hands the native tool `-j` with no number,
// which GNU make reads as "unlimited": every llama.cpp translation unit
// compiles at once (170+ cc1plus processes, ~100 MB each), so a 16 GB
// machine runs out of memory and the build dies with "Killed signal
// terminated program cc1plus". CI already passes an explicit count
// (`-j "$(nproc)"`); this does the same portably, without needing a POSIX
// shell for `$(nproc)`. Override with ONYX_BUILD_JOBS=N.
'use strict';

const { execFileSync } = require('child_process');
const os = require('os');

const cpus = typeof os.availableParallelism === 'function'
  ? os.availableParallelism()
  : os.cpus().length;
const jobs = String(Number(process.env.ONYX_BUILD_JOBS) || Math.max(1, cpus));

const run = (args) => execFileSync('cmake', args, { stdio: 'inherit' });

run(['-B', 'build', '-DCMAKE_BUILD_TYPE=Release']);
run(['--build', 'build', '--target', 'onyx-engine', '-j', jobs]);
