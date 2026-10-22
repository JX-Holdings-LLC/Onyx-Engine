#!/usr/bin/env node
// Checks for the nightly QA runner (.ai/qa.yaml), in Node so the same commands
// run under the Linux/macOS shells and the Windows runner's cmd.exe:
//
//   node scripts/tools/qa-check.js vendor    the pinned llama.cpp source is installed
//   node scripts/tools/qa-check.js build     `npm run build`, when cmake is installed
//   node scripts/tools/qa-check.js binary    the built onyx-engine runs --version
//   node scripts/tools/qa-check.js runtime   it loads the tiny test model and serves requests
//   node scripts/tools/qa-check.js smoke     the full smoke suite (scripts/smoke-test.sh)
//
// When this machine lacks a prerequisite (cmake, a build, Python with numpy, a
// POSIX shell), the check prints `QA_CHECK_SKIP: <reason>` and exits 0, which
// the runner records as SKIP rather than a pass or a failure. The runner only
// honours that line from commands under scripts/tools/, hence this location.
'use strict';

const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const { readSpec } = require('../install-vendor');

const root = path.resolve(__dirname, '..', '..');
// Where a single-config (make/ninja) or a Visual Studio build leaves the binary.
const BINARIES = ['build/onyx-engine', 'build/onyx-engine.exe', 'build/Release/onyx-engine.exe'];
const MODEL = path.join(root, 'models-test', 'tiny-llama-random.gguf');
const PYTHON_DEPS = 'Python with numpy, pyyaml, tqdm and requests';

function skip(reason) {
  console.log(`QA_CHECK_SKIP: ${reason}`);
  process.exit(0);
}

function fail(message) {
  console.error(`FAIL: ${message}`);
  process.exit(1);
}

function has(command, args = ['--version']) {
  return spawnSync(command, args, { stdio: 'ignore' }).status === 0;
}

function findBinary() {
  for (const rel of BINARIES) {
    const file = path.join(root, rel);
    if (fs.existsSync(file)) return file;
  }
  return null;
}

// The first Python that can import numpy and the vendored gguf-py, which is
// what scripts/make-tiny-model.py needs to write the test model.
function findPython() {
  const ggufPy = path.join(root, 'node_modules', '@jxburros', 'llama-cpp-source', 'gguf-py');
  const probe = 'import sys; sys.path.insert(0, sys.argv[1]); import numpy, gguf';
  const candidates = process.platform === 'win32'
    ? [['python'], ['py', '-3'], ['python3']]
    : [['python3'], ['python']];
  return candidates.find(([cmd, ...pre]) => has(cmd, [...pre, '-c', probe, ggufPy])) ?? null;
}

function vendor() {
  const spec = readSpec('llama.cpp');
  const dir = path.join(root, 'node_modules', ...spec.pkgName.split('/'));
  const stamp = path.join(dir, '.vendor-source-commit');
  if (!fs.existsSync(stamp)) fail(`${spec.pkgName} is not installed (run: npm ci)`);
  const installed = fs.readFileSync(stamp, 'utf8').trim();
  if (installed !== spec.commit) {
    fail(`${spec.pkgName} is at ${installed}, but packaging/vendor/llama.cpp.conf pins ${spec.commit} (run: npm ci)`);
  }
  const missing = spec.required.filter((p) => !fs.existsSync(path.join(dir, p)));
  if (missing.length) fail(`${spec.pkgName} is missing ${missing.join(', ')}`);
  console.log(`ok: ${spec.name} ${spec.commit} is installed`);
}

function build() {
  if (!has('cmake')) skip('cmake is not installed on this runner');
  const r = spawnSync(process.execPath, [path.join(root, 'scripts', 'build.js')], { cwd: root, stdio: 'inherit' });
  if (r.status !== 0) fail(`npm run build exited ${r.status}`);
}

function binary() {
  const bin = findBinary();
  if (!bin) fail(`no onyx-engine binary at ${BINARIES.join(', ')} (run: npm run build)`);
  const r = spawnSync(bin, ['--version'], { encoding: 'utf8' });
  const out = `${r.stdout ?? ''}${r.stderr ?? ''}`.trim();
  if (r.status !== 0 || !out.includes('onyx-engine')) fail(`${bin} --version exited ${r.status}: ${out}`);
  console.log(`ok: ${out}`);
}

function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close(() => resolve(port));
    });
  });
}

async function request(url, body) {
  const res = await fetch(url, body === undefined ? {} : {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`${url} answered ${res.status}: ${await res.text()}`);
  return res.json();
}

async function runtime() {
  const bin = findBinary();
  if (!bin) skip('there is no onyx-engine build to start (Build Health builds it)');
  if (!fs.existsSync(MODEL)) {
    const python = findPython();
    if (!python) skip(`the tiny test model needs ${PYTHON_DEPS}`);
    const [cmd, ...pre] = python;
    const r = spawnSync(cmd, [...pre, path.join(root, 'scripts', 'make-tiny-model.py'), MODEL], { stdio: 'inherit' });
    if (r.status !== 0) fail('scripts/make-tiny-model.py could not write the test model');
  }

  const port = await freePort();
  const base = `http://127.0.0.1:${port}`;
  const log = [];
  let exited = null;
  const child = spawn(bin, ['-m', MODEL, '--port', String(port), '-c', '512', '--alias', 'tiny-qa'], { cwd: root });
  child.stdout.on('data', (d) => log.push(d));
  child.stderr.on('data', (d) => log.push(d));
  child.on('exit', (code) => { exited = code ?? 'signal'; });

  let error = null;
  try {
    // /health answers 503 while the model loads and 200 once it can serve.
    const deadline = Date.now() + 60_000;
    for (;;) {
      if (exited !== null) throw new Error(`onyx-engine exited (${exited}) before it was ready`);
      const status = await fetch(`${base}/health`).then((r) => r.status, () => 0);
      if (status === 200) break;
      if (Date.now() > deadline) throw new Error(`/health did not answer 200 within 60 s (last status ${status})`);
      await new Promise((r) => setTimeout(r, 250));
    }
    const models = await request(`${base}/v1/models`);
    if (models.data?.[0]?.id !== 'tiny-qa') throw new Error(`/v1/models does not list the alias: ${JSON.stringify(models)}`);
    const chat = await request(`${base}/v1/chat/completions`, {
      messages: [{ role: 'user', content: 'hello' }],
      max_tokens: 4,
    });
    if (!Array.isArray(chat.choices) || chat.choices.length === 0) {
      throw new Error(`chat completion has no choices: ${JSON.stringify(chat)}`);
    }
  } catch (e) {
    error = e;
  } finally {
    child.kill();
  }
  if (error) {
    console.error(Buffer.concat(log).toString('utf8').split('\n').slice(-30).join('\n'));
    fail(error.message);
  }
  console.log(`ok: ${path.relative(root, bin)} served /health, /v1/models and a chat completion`);
}

function smoke() {
  if (process.platform === 'win32') {
    skip('the smoke suite is POSIX shell; CI runs it on Linux and macOS, and Runtime Health covers Windows');
  }
  const bin = findBinary();
  if (!bin) skip('there is no onyx-engine build to test (Build Health builds it)');
  for (const tool of ['bash', 'curl']) if (!has(tool)) skip(`${tool} is not installed`);
  // scripts/smoke-test.sh calls python3 by that name.
  if (findPython()?.[0] !== 'python3') skip(`the smoke suite needs python3 with numpy, pyyaml, tqdm and requests`);
  const r = spawnSync('bash', ['scripts/smoke-test.sh', bin], { cwd: root, stdio: 'inherit' });
  process.exit(r.status ?? 1);
}

const checks = { vendor, build, binary, runtime, smoke };
const check = checks[process.argv[2]];
if (!check) {
  console.error(`usage: node scripts/tools/qa-check.js <${Object.keys(checks).join('|')}>`);
  process.exit(2);
}
Promise.resolve(check()).catch((e) => fail(e.stack ?? String(e)));
