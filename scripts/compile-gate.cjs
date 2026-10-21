'use strict';
// compile-gate.cjs: does the C++ a change touched still compile? converge runs it on every submission and every train.
//
//   CONVERGE_BASE=<commit> node scripts/compile-gate.cjs       in a checkout of the commit to check
//
// Every .cpp the change added or modified, and every .cpp that includes (directly or through other headers) a header it
// changed, is type-checked with g++ -fsyntax-only: no code generation, no link. A file that fails is checked again at
// the base commit; one that fails there too was already broken and is reported but not held against the change.
//
// It fails closed: no g++, a compile that never ran, or a base it cannot read is a failure, never a pass. Node, not sh:
// converge runs checks under aios-sh, whose builtins are not GNU's (the shell version of this gate passed without
// compiling anything there). Compiles go through the machine's build slots (/var/tmp/aios-build-slots, see
// scripts/build-slot.sh) at nice 19 / idle I/O, so the checks never starve the box.
const { execFileSync, spawn, spawnSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const say = (s) => process.stdout.write(s + '\n');
const fail = (s) => { say('FAIL gate: ' + s); process.exit(1); };
const git = (args, opts = {}) => execFileSync('git', args, { encoding: 'utf8', maxBuffer: 256 << 20, ...opts });

const base = process.env.CONVERGE_BASE;
if (!base) { say('PASS gate: no base commit was given: nothing to compare with'); process.exit(0); }
try { process.chdir(git(['rev-parse', '--show-toplevel']).trim()); } catch (e) { fail('not in a git checkout'); }
try { git(['cat-file', '-e', base + '^{commit}']); } catch { fail('base ' + base.slice(0, 8) + ' is not in this checkout, so the change cannot be compared'); }
const gxx = spawnSync('g++', ['--version'], { encoding: 'utf8' });
if (gxx.error || gxx.status !== 0) fail('no working g++ here, so the C++ was not checked');

const SLOTS = Number(process.env.AIOS_BUILD_SLOTS || 2);
const SLOT_DIR = '/var/tmp/aios-build-slots';
const haveFlock = !spawnSync('flock', ['-V']).error && fs.existsSync(SLOT_DIR + '/slot0');
const FLAGS = ['-std=c++20', '-fsyntax-only', '-w', '-Iinclude', '-Ithird_party/tree-sitter/lib/include', '-I/usr/include/SDL2',
  '-DAIOS_HAS_SDL2=1', '-DAIOS_HAVE_LIBDEFLATE=1', '-DAIOS_HAVE_ZLIB=1', '-DAIOS_HAVE_ZSTD=1'];
const SKIP = /^(third_party|build)\/|(^|\/)font_atlas\.cpp$/;
const TU = /\.(cpp|cc)$/;
const HDR = /\.(hpp|hh|h|inc|ipp)$/;

const changed = git(['diff', '--name-only', '--diff-filter=AM', '-z', base, 'HEAD']).split('\0').filter(Boolean);

// Who includes whom, by file name: enough to follow a header to every translation unit that sees it.
const tracked = git(['ls-files', '-z', '--', 'src', 'include', 'tools', 'cli', 'metal']).split('\0').filter((f) => f && (TU.test(f) || HDR.test(f)) && !SKIP.test(f));
const includers = new Map();   // header base name -> files that include it
const INC = /^[ \t]*#[ \t]*include[ \t]*["<]([^">]+)[">]/gm;
for (const f of tracked) {
  let text; try { text = fs.readFileSync(f, 'utf8'); } catch { continue; }
  for (const m of text.matchAll(INC)) {
    const b = path.basename(m[1]);
    if (!includers.has(b)) includers.set(b, new Set());
    includers.get(b).add(f);
  }
}
const tus = new Set(changed.filter((f) => TU.test(f) && !SKIP.test(f) && fs.existsSync(f)));
const direct = tus.size;
const seen = new Set();
const queue = changed.filter((f) => HDR.test(f) && !SKIP.test(f));
while (queue.length) {
  const h = queue.shift();
  const b = path.basename(h);
  if (seen.has(b)) continue;
  seen.add(b);
  for (const f of includers.get(b) || []) {
    if (TU.test(f)) tus.add(f);
    else if (HDR.test(f)) queue.push(f);
  }
}
const list = [...tus];
if (!list.length) { say(`PASS gate: no C++ translation unit is touched by this change (${changed.length} files changed)`); process.exit(0); }
say(`gate: ${list.length} translation unit(s) to check (${direct} changed, ${list.length - direct} through ${seen.size} changed header(s)), ${SLOTS} at a time${haveFlock ? ' in the build slots' : ''}`);

function compile(file, cwd) {
  return new Promise((resolve) => {
    const argv = ['nice', '-n', '19', 'ionice', '-c', '3', 'g++', ...FLAGS, file];
    let cmd = argv;
    if (haveFlock) cmd = ['flock', `${SLOT_DIR}/slot${compile.n++ % SLOTS}`, ...argv];
    const p = spawn(cmd[0], cmd.slice(1), { cwd, stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    p.stderr.on('data', (d) => { if (err.length < 64 << 10) err += d; });
    p.on('error', (e) => resolve({ file, ok: false, err: String(e.message) }));
    p.on('close', (code) => resolve({ file, ok: code === 0, err }));
  });
}
compile.n = 0;

async function run(files, cwd) {
  const out = [];
  let i = 0;
  const worker = async () => { while (i < files.length) { const f = files[i++]; out.push(await compile(f, cwd)); } };
  await Promise.all(Array.from({ length: Math.max(1, SLOTS) }, worker));
  return out;
}

(async () => {
  const t0 = Date.now();
  const results = await run(list, process.cwd());
  if (results.length !== list.length) fail(`only ${results.length} of ${list.length} compiles ran`);
  const bad = results.filter((r) => !r.ok);
  if (!bad.length) { say(`PASS gate: ${list.length} C++ file(s) compile (${changed.length} changed, ${((Date.now() - t0) / 1000).toFixed(1)}s)`); return; }

  // Which failures are this change's? The same files at the base commit, in a worktree of it.
  const dir = fs.mkdtempSync(path.join(process.env.TMPDIR || os.tmpdir(), 'gate-base-'));
  let baseOk = true;
  try { git(['worktree', 'add', '--detach', '-f', dir, base], { stdio: 'ignore' }); } catch { baseOk = false; }
  const old = new Set();
  if (baseOk) {
    const again = await run(bad.map((r) => r.file).filter((f) => fs.existsSync(path.join(dir, f))), dir);
    for (const r of again) if (!r.ok) old.add(r.file);
    try { git(['worktree', 'remove', '--force', dir], { stdio: 'ignore' }); } catch { /* left for the workspace's cleanup */ }
  }
  const mine = bad.filter((r) => !old.has(r.file));
  for (const r of mine) {
    say('== ' + r.file);
    for (const l of r.err.split('\n').filter((l) => /error/.test(l)).slice(0, 6)) say('   ' + l);
  }
  for (const f of old) say(`(already failing at ${base.slice(0, 8)}, not held against this change: ${f})`);
  if (mine.length) fail(`${mine.length} C++ file(s) no longer compile (${list.length} checked${baseOk ? '' : '; the base could not be read, so every failure counts'})`);
  say(`PASS gate: ${list.length} checked, ${old.size} already failing at the base`);
})().catch((e) => fail('the gate itself failed: ' + (e && e.stack || e)));
