// aios-git over the wire, both ways round.
//
//   node tools/proof/aios-git/transport.mjs <aios-git> [--git <real git>] [--no-github]
//
// 1. aios-git as the server (`git http-backend`, which converge's local git host runs): real git
//    clones, fetches and pushes through it (new branch, fast-forward, forced update, delete, tags).
// 2. real git as the server: aios-git does the same as the client.
// 3. aios-git against GitHub over https: clone, shallow clone and ls-remote of a public repository,
//    held against what real git gets.
// Every repository either side ends up with is checked by real git: fsck, refs, and the work tree.
// Both servers demand an Authorization header, sent the way converge sends it (-c http.extraHeader).
import { spawn, spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const args = process.argv.slice(2);
// The server runs in a process of its own: the clients below are run synchronously, which would
// otherwise stop the server answering them.
const SERVE = args[0] === '--serve';
const AIOS = SERVE ? args[1] : path.resolve(args[0] || '');
const REAL = args.includes('--git') ? args[args.indexOf('--git') + 1] : 'git';
const GITHUB = !args.includes('--no-github');
if (!SERVE && !fs.existsSync(AIOS)) { console.error('usage: transport.mjs <aios-git> [--git <git>] [--no-github]'); process.exit(2); }

const ROOT = SERVE ? path.dirname(args[2]) : fs.mkdtempSync(path.join(os.tmpdir(), 'aios-git-wire-'));
const HOME = path.join(ROOT, 'home');
fs.mkdirSync(HOME, { recursive: true });
const TOKEN = 'wire-test-token';
const AUTH = `Authorization: Basic ${Buffer.from(`x:${TOKEN}`).toString('base64')}`;
const env = {
  ...Object.fromEntries(Object.entries(process.env).filter(([k]) => !k.startsWith('GIT_'))),
  HOME, USERPROFILE: HOME, GIT_CONFIG_NOSYSTEM: '1', GIT_TERMINAL_PROMPT: '0',
  GIT_AUTHOR_NAME: 'A', GIT_AUTHOR_EMAIL: 'a@x', GIT_COMMITTER_NAME: 'C', GIT_COMMITTER_EMAIL: 'c@x',
  GIT_AUTHOR_DATE: '1700000000 +0000', GIT_COMMITTER_DATE: '1700000000 +0000',
};
const run = (bin, argv, cwd, input) => {
  const r = spawnSync(bin, argv, { cwd, env, input, encoding: 'utf8', timeout: 180_000 });
  return { code: r.status, out: (r.stdout || '').trim(), err: (r.stderr || '').trim() };
};
const real = (argv, cwd) => run(REAL, argv, cwd);
const must = (r, what) => { if (r.code !== 0) throw new Error(`${what} failed (${r.code}): ${r.err || r.out}`); return r; };

// The server in a child process; resolves with something that has address().port and close().
function serve(backend, projectRoot) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1'), '--serve', backend, projectRoot],
      { stdio: ['ignore', 'pipe', 'inherit'] });
    let buf = '';
    child.stdout.on('data', (d) => {
      buf += d;
      const nl = buf.indexOf('\n');
      if (nl < 0) return;
      const port = Number(buf.slice(0, nl));
      resolve({ address: () => ({ port }), close: () => child.kill() });
    });
    child.on('error', reject);
  });
}

// A CGI bridge like converge's scripts/lib/git-host.mjs, for either git as the backend.
function serveInProcess(backend, projectRoot) {
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      if (req.headers.authorization !== AUTH.slice('Authorization: '.length)) {
        res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="t"' });
        return res.end('auth required');
      }
      const url = new URL(req.url, 'http://x');
      const cgiEnv = {
        PATH: process.env.PATH, SystemRoot: process.env.SystemRoot || '', GIT_PROJECT_ROOT: projectRoot, GIT_HTTP_EXPORT_ALL: '1',
        PATH_INFO: decodeURIComponent(url.pathname), REQUEST_METHOD: req.method, QUERY_STRING: url.search.slice(1),
        CONTENT_TYPE: req.headers['content-type'] || '', HTTP_CONTENT_ENCODING: req.headers['content-encoding'] || '',
        GIT_PROTOCOL: req.headers['git-protocol'] || '', REMOTE_USER: 'tester', REMOTE_ADDR: '127.0.0.1',
        GIT_CONFIG_COUNT: '1', GIT_CONFIG_KEY_0: 'http.receivepack', GIT_CONFIG_VALUE_0: 'true',
        GIT_CONFIG_NOSYSTEM: '1', HOME, USERPROFILE: HOME, TEMP: process.env.TEMP || '', TMP: process.env.TMP || '',
      };
      const cgi = spawn(backend, ['http-backend'], { env: cgiEnv, stdio: ['pipe', 'pipe', 'pipe'] });
      req.pipe(cgi.stdin);
      let head = Buffer.alloc(0), done = false, errText = '';
      cgi.stdout.on('data', (chunk) => {
        if (done) return res.write(chunk);
        head = Buffer.concat([head, chunk]);
        let end = head.indexOf('\r\n\r\n'), sep = 4;
        if (end < 0) { end = head.indexOf('\n\n'); sep = 2; }
        if (end < 0) return;
        const headers = {};
        let status = 200;
        for (const line of head.subarray(0, end).toString().split(/\r?\n/)) {
          const i = line.indexOf(':');
          if (i < 0) continue;
          const k = line.slice(0, i).trim(), v = line.slice(i + 1).trim();
          if (k.toLowerCase() === 'status') status = parseInt(v, 10) || 200;
          else headers[k] = v;
        }
        res.writeHead(status, headers);
        done = true;
        const body = head.subarray(end + sep);
        if (body.length) res.write(body);
      });
      cgi.stderr.on('data', (d) => { errText += d; });
      cgi.on('close', (code) => {
        if (done) return res.end();
        res.writeHead(502);
        res.end(`backend exited ${code}: ${errText}`);
      });
    });
    server.listen(0, '127.0.0.1', () => resolve(server));
  });
}

function snapshot(dir) {
  const files = {};
  const walk = (d, rel) => {
    for (const n of fs.readdirSync(d)) {
      if (n === '.git' && !rel) continue;
      const full = path.join(d, n), r = rel ? `${rel}/${n}` : n;
      if (fs.statSync(full).isDirectory()) walk(full, r);
      else files[r] = crypto.createHash('sha1').update(fs.readFileSync(full)).digest('hex');
    }
  };
  if (fs.existsSync(path.join(dir, '.git'))) walk(dir, '');
  return {
    refs: real(['for-each-ref', '--format=%(objectname) %(refname)'], dir).out,
    head: real(['rev-parse', 'HEAD'], dir).out,
    fsck: real(['fsck', '--strict', '--no-dangling'], dir),
    status: fs.existsSync(path.join(dir, '.git')) ? real(['status', '--porcelain'], dir).out : '',
    files,
  };
}

const results = [];
function check(name, fn) {
  try {
    fn();
    results.push(`ok   ${name}`);
  } catch (e) {
    results.push(`FAIL ${name}\n     ${String(e.message).split('\n').join('\n     ')}`);
  }
}
function same(a, b, what) {
  if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${what} differs:\n  one:   ${JSON.stringify(a).slice(0, 600)}\n  other: ${JSON.stringify(b).slice(0, 600)}`);
}

// A source repository with some history, branches and tags.
function seed(dir) {
  fs.mkdirSync(dir, { recursive: true });
  must(real(['init', '-q', '-b', 'main'], dir), 'init');
  for (let i = 0; i < 5; i++) {
    fs.mkdirSync(path.join(dir, 'src', `m${i % 2}`), { recursive: true });
    fs.writeFileSync(path.join(dir, 'src', `m${i % 2}`, `f${i}.txt`), `${'content '.repeat(200)}${i}\n`);
    fs.writeFileSync(path.join(dir, 'big.bin'), crypto.randomBytes(100_000 + i));
    must(real(['add', '-A'], dir), 'add');
    must(real(['commit', '-qm', `c${i}`], dir), 'commit');
  }
  must(real(['branch', 'side', 'HEAD~2'], dir), 'branch');
  must(real(['tag', 'v1', 'HEAD~1'], dir), 'tag');
  must(real(['tag', '-a', 'v2', '-m', 'annotated'], dir), 'tag -a');
}

async function main() {
  const srcRoot = path.join(ROOT, 'src');
  seed(path.join(srcRoot, 'work'));

  for (const [label, serverGit, clientGit] of [['aios-git server, real git client', AIOS, REAL], ['real git server, aios-git client', REAL, AIOS]]) {
    const proj = path.join(ROOT, label.replace(/[^a-z]+/gi, '_'));
    fs.mkdirSync(proj, { recursive: true });
    must(real(['clone', '-q', '--bare', path.join(srcRoot, 'work'), path.join(proj, 'repo.git')]), 'bare clone');
    const server = await serve(serverGit, proj);
    const url = `http://127.0.0.1:${server.address().port}/repo.git`;
    const client = (argv, cwd) => run(clientGit, ['-c', `http.extraHeader=${AUTH}`, ...argv], cwd);
    const c1 = path.join(proj, 'c1');
    check(`${label}: clone without the header is refused`, () => {
      const r = run(clientGit, ['clone', '-q', url, path.join(proj, 'noauth')]);
      if (r.code === 0) throw new Error('clone succeeded without authorization');
    });
    check(`${label}: clone`, () => {
      must(client(['clone', '-q', url, c1]), 'clone');
      const ref = path.join(proj, 'ref');
      must(real(['clone', '-q', path.join(proj, 'repo.git'), ref]), 'reference clone');
      const a = snapshot(c1), b = snapshot(ref);
      if (a.fsck.code !== 0) throw new Error(`fsck: ${a.fsck.err}`);
      same(a.files, b.files, 'work tree');
      same(a.head, b.head, 'HEAD');
      same(a.refs.replace(/refs\/remotes\/origin\/HEAD\n?/, ''), b.refs.replace(/refs\/remotes\/origin\/HEAD\n?/, ''), 'refs');
    });
    check(`${label}: push a new branch, then fast-forward it`, () => {
      must(real(['checkout', '-qb', 'feature'], c1), 'checkout');
      fs.writeFileSync(path.join(c1, 'new.txt'), 'new\n');
      must(real(['add', 'new.txt'], c1), 'add');
      must(real(['commit', '-qm', 'feature work'], c1), 'commit');
      must(client(['push', '-q', 'origin', 'HEAD:refs/heads/feature'], c1), 'push new');
      fs.appendFileSync(path.join(c1, 'new.txt'), 'more\n');
      must(real(['commit', '-qam', 'more'], c1), 'commit');
      must(client(['push', '-q', 'origin', 'feature'], c1), 'push ff');
      same(real(['rev-parse', 'refs/heads/feature'], path.join(proj, 'repo.git')).out, real(['rev-parse', 'HEAD'], c1).out, 'server feature');
      const f = real(['fsck', '--strict', '--no-dangling'], path.join(proj, 'repo.git'));
      if (f.code !== 0) throw new Error(`server fsck: ${f.err}`);
    });
    check(`${label}: non-fast-forward refused, then forced; lease`, () => {
      must(real(['reset', '-q', '--hard', 'HEAD~1'], c1), 'reset');
      fs.writeFileSync(path.join(c1, 'other.txt'), 'x\n');
      must(real(['add', 'other.txt'], c1), 'add');
      must(real(['commit', '-qm', 'diverge'], c1), 'commit');
      const r = client(['push', 'origin', 'feature'], c1);
      if (r.code === 0) throw new Error('non-fast-forward push accepted');
      const serverWas = real(['rev-parse', 'refs/heads/feature'], path.join(proj, 'repo.git')).out;
      const stale = client(['push', `--force-with-lease=feature:${'1'.repeat(40)}`, 'origin', 'feature'], c1);
      if (stale.code === 0) throw new Error('push with a stale lease accepted');
      must(client(['push', '-q', `--force-with-lease=feature:${serverWas}`, 'origin', 'feature'], c1), 'push with lease');
      same(real(['rev-parse', 'refs/heads/feature'], path.join(proj, 'repo.git')).out, real(['rev-parse', 'HEAD'], c1).out, 'server feature');
    });
    check(`${label}: push a commit to a custom ref, delete a branch, push a tag`, () => {
      must(client(['push', '-q', '--force', 'origin', 'HEAD:refs/converge/cycles/1'], c1), 'push sha');
      must(client(['push', '-q', 'origin', ':feature'], c1), 'delete');
      must(real(['tag', 'v3'], c1), 'tag');
      must(client(['push', '-q', 'origin', 'v3'], c1), 'push tag');
      const refs = real(['for-each-ref', '--format=%(refname)'], path.join(proj, 'repo.git')).out.split('\n');
      if (refs.includes('refs/heads/feature')) throw new Error('feature not deleted');
      if (!refs.includes('refs/converge/cycles/1') || !refs.includes('refs/tags/v3')) throw new Error(`refs: ${refs}`);
    });
    check(`${label}: fetch what someone else pushed, FETCH_HEAD, merge`, () => {
      const other = path.join(proj, 'other');
      must(real(['clone', '-q', path.join(proj, 'repo.git'), other]), 'clone other');
      fs.writeFileSync(path.join(other, 'theirs.txt'), 't\n');
      must(real(['add', 'theirs.txt'], other), 'add');
      must(real(['commit', '-qm', 'theirs'], other), 'commit');
      must(real(['push', '-q', 'origin', 'main'], other), 'push other');
      must(real(['checkout', '-q', 'main'], c1), 'checkout main');
      must(client(['fetch', '-q', 'origin', 'main'], c1), 'fetch');
      same(real(['rev-parse', 'FETCH_HEAD'], c1).out, real(['rev-parse', 'HEAD'], other).out, 'FETCH_HEAD');
      same(real(['rev-parse', 'refs/remotes/origin/main'], c1).out, real(['rev-parse', 'HEAD'], other).out, 'origin/main');
      must(real(['merge', '-q', '--ff-only', 'FETCH_HEAD'], c1), 'merge');
      const f = real(['fsck', '--strict', '--no-dangling'], c1);
      if (f.code !== 0) throw new Error(`client fsck: ${f.err}`);
    });
    check(`${label}: ls-remote`, () => {
      const a = client(['ls-remote', url], c1).out, b = run(REAL, ['ls-remote', path.join(proj, 'repo.git')]).out;
      same(a.split('\n').filter((l) => !l.endsWith('\tHEAD')).sort(), b.split('\n').filter((l) => !l.endsWith('\tHEAD')).sort(), 'ls-remote');
    });
    // Shallow over smart HTTP is several stateless requests that must each carry the shallow list
    // (converge's planner and agents clone with --depth). The same steps against the repository over
    // file:// with real git are the reference: commits, shallow commits and fsck must agree.
    check(`${label}: shallow clone, deepen, fetch new work, unshallow`, () => {
      const s = path.join(proj, 'shallow'), ref = path.join(proj, 'shallow-ref');
      const fileUrl = `file://${path.join(proj, 'repo.git').replace(/\\/g, '/')}`;
      const state = (dir) => ({
        commits: real(['rev-list', 'HEAD'], dir).out,
        shallow: fs.existsSync(path.join(dir, '.git', 'shallow')) ? fs.readFileSync(path.join(dir, '.git', 'shallow'), 'utf8').split('\n').filter(Boolean).sort() : [],
      });
      const step = (what, argv) => {
        must(client(argv(url), argv === cloneArgs ? undefined : s), `${what} (http)`);
        must(real(argv(fileUrl), argv === cloneArgs ? undefined : ref), `${what} (reference)`);
        const a = state(s), b = state(ref);
        same(a, b, `after ${what}`);
        const f = real(['fsck', '--strict', '--no-dangling'], s);
        if (f.code !== 0) throw new Error(`fsck after ${what}: ${f.err}`);
      };
      const cloneArgs = (u) => ['clone', '-q', '--depth', '1', u, u === url ? s : ref];
      step('clone --depth 1', cloneArgs);
      step('fetch --depth 3', (u) => ['fetch', '-q', '--depth', '3', u, 'main']);
      step('fetch --deepen 1', (u) => ['fetch', '-q', '--deepen', '1', u, 'main']);
      const other = path.join(proj, 'other');
      fs.writeFileSync(path.join(other, 'later.txt'), 'later\n');
      must(real(['add', 'later.txt'], other), 'add');
      must(real(['commit', '-qm', 'later'], other), 'commit');
      must(real(['push', '-q', 'origin', 'main'], other), 'push other');
      for (const dir of [s, ref]) must(real(['merge', '-q', '--ff-only', 'FETCH_HEAD'], dir), 'merge');
      step('fetch new work', (u) => ['fetch', '-q', u, 'main']);
      for (const dir of [s, ref]) must(real(['merge', '-q', '--ff-only', 'FETCH_HEAD'], dir), 'merge');
      step('fetch --unshallow', (u) => ['fetch', '-q', '--unshallow', u, 'main']);
    });
    server.close();
  }

  if (GITHUB) {
    const gh = 'https://github.com/octocat/Hello-World.git';
    const a = path.join(ROOT, 'gh-aios'), b = path.join(ROOT, 'gh-real');
    check('GitHub https: clone', () => {
      must(run(AIOS, ['clone', '-q', gh, a]), 'aios clone');
      must(real(['clone', '-q', gh, b]), 'real clone');
      const sa = snapshot(a), sb = snapshot(b);
      if (sa.fsck.code !== 0) throw new Error(`fsck: ${sa.fsck.err}`);
      same(sa.head, sb.head, 'HEAD');
      same(sa.files, sb.files, 'work tree');
      same(sa.refs.replace(/^.*refs\/remotes\/origin\/HEAD$\n?/m, ''), sb.refs.replace(/^.*refs\/remotes\/origin\/HEAD$\n?/m, ''), 'refs');
      same(sa.status, '', 'status');
    });
    check('GitHub https: shallow clone', () => {
      const s = path.join(ROOT, 'gh-shallow');
      must(run(AIOS, ['clone', '-q', '--depth', '1', gh, s]), 'aios shallow clone');
      same(real(['rev-parse', 'HEAD'], s).out, real(['rev-parse', 'HEAD'], b).out, 'HEAD');
      same(real(['rev-list', '--count', 'HEAD'], s).out, '1', 'history depth');
      same(real(['rev-parse', '--is-shallow-repository'], s).out, 'true', 'shallow');
    });
    check('GitHub https: ls-remote', () => {
      same(run(AIOS, ['ls-remote', '--heads', gh]).out, real(['ls-remote', '--heads', gh]).out, 'ls-remote --heads');
    });
    check('GitHub https: fetch into an existing clone', () => {
      must(run(AIOS, ['fetch', '-q', 'origin'], a), 'fetch');
      must(run(AIOS, ['fetch', '-q', 'origin', 'test'], a), 'fetch one branch');
      same(real(['rev-parse', 'FETCH_HEAD'], a).out, real(['rev-parse', 'refs/remotes/origin/test'], b).out, 'FETCH_HEAD');
    });
  }

  for (const l of results) console.log(l);
  const failed = results.filter((l) => l.startsWith('FAIL')).length;
  console.log(`\n${results.length - failed}/${results.length} transport checks pass`);
  fs.rmSync(ROOT, { recursive: true, force: true });
  process.exit(failed ? 1 : 0);
}
if (SERVE) {
  const s = await serveInProcess(AIOS, args[2]);
  console.log(s.address().port);
} else {
  main().catch((e) => { console.error(e); process.exit(1); });
}
