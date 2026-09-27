// Fetch the exact upstream commit without depending on an unpublished npm package.
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '..');
const pinFile = fs.readFileSync(path.join(root, 'packaging', 'make-vendor-package.sh'), 'utf8');
const match = pinFile.match(/^LLAMA_COMMIT="([0-9a-f]{40})"/m);
if (!match) throw new Error('llama.cpp commit pin is missing');
const commit = match[1];
const destination = path.join(root, 'node_modules', '@jxburros', 'llama-cpp-source');
const stamp = path.join(destination, '.onyx-source-commit');
if (fs.existsSync(stamp) && fs.readFileSync(stamp, 'utf8').trim() === commit &&
    fs.existsSync(path.join(destination, 'CMakeLists.txt'))) {
  console.log(`llama.cpp ${commit} is already installed`);
  process.exit(0);
}

fs.rmSync(destination, { recursive: true, force: true });
fs.mkdirSync(destination, { recursive: true });
function git(...args) {
  execFileSync('git', args, { cwd: destination, stdio: 'inherit' });
}
try {
  git('init', '-q');
  git('remote', 'add', 'origin', 'https://github.com/ggml-org/llama.cpp.git');
  git('sparse-checkout', 'init', '--cone');
  git('sparse-checkout', 'set', 'cmake', 'common', 'src', 'ggml', 'include', 'tools/mtmd', 'vendor', 'models');
  git('fetch', '--depth', '1', 'origin', commit);
  git('checkout', '-q', 'FETCH_HEAD');
  const actual = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: destination, encoding: 'utf8' }).trim();
  if (actual !== commit) throw new Error(`unexpected llama.cpp commit ${actual}`);
  fs.rmSync(path.join(destination, '.git'), { recursive: true, force: true });
  fs.writeFileSync(stamp, `${commit}\n`);
} catch (error) {
  fs.rmSync(destination, { recursive: true, force: true });
  throw error;
}
