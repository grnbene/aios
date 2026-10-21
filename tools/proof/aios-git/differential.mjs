// aios-git held against real git, strictly.
//
//   node tools/proof/aios-git/differential.mjs <aios-git> [--git <real git>] [--only <substring>] [--verbose] [--keep]
//
// Every case is a list of steps (file changes and git commands) run twice, in two fresh directories:
// once with real git, once with aios-git, under the same pinned identities and dates, so even commit ids
// must come out the same. For each git step the exit code and stdout must be identical (stderr too, for
// steps marked `err: true`). After the last step real git itself inspects both repositories: HEAD, every
// ref, the index (ls-files -s), status, the work tree's files, and `git fsck --strict` on aios-git's side.
// Exit 0 only when every case agrees.
import { spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const args = process.argv.slice(2);
const AIOS = args[0] && path.resolve(args[0]);
const REAL = args.includes('--git') ? args[args.indexOf('--git') + 1] : 'git';
const ONLY = args.includes('--only') ? args[args.indexOf('--only') + 1] : '';
const VERBOSE = args.includes('--verbose');
const KEEP = args.includes('--keep');
if (!AIOS || !fs.existsSync(AIOS)) {
  console.error('usage: differential.mjs <aios-git> [--git <git>] [--only <substring>] [--verbose] [--keep]');
  process.exit(2);
}

const ROOT = fs.mkdtempSync(path.join(os.tmpdir(), 'aios-git-diff-'));
const HOME = path.join(ROOT, 'home');
fs.mkdirSync(HOME);
let clock = 1700000000;

function envFor(home, extra = {}) {
  const e = { ...process.env };
  for (const k of Object.keys(e)) if (k.startsWith('GIT_')) delete e[k];
  return {
    ...e,
    HOME: home, USERPROFILE: home, XDG_CONFIG_HOME: path.join(home, '.config'),
    GIT_CONFIG_NOSYSTEM: '1', GIT_TERMINAL_PROMPT: '0', LANG: 'C', LC_ALL: 'C',
    GIT_AUTHOR_NAME: 'A U Thor', GIT_AUTHOR_EMAIL: 'author@example.com',
    GIT_COMMITTER_NAME: 'C O Mitter', GIT_COMMITTER_EMAIL: 'committer@example.com',
    GIT_AUTHOR_DATE: `${clock} +0000`, GIT_COMMITTER_DATE: `${clock} +0000`,
    GIT_EDITOR: 'true', GIT_PAGER: 'cat', PAGER: 'cat',
    ...extra,
  };
}

// ---------------------------------------------------------------------------
// step helpers
// ---------------------------------------------------------------------------
const W = (file, text) => ({ fs: (d) => { fs.mkdirSync(path.dirname(path.join(d, file)), { recursive: true }); fs.writeFileSync(path.join(d, file), text); } });
const RM = (file) => ({ fs: (d) => fs.rmSync(path.join(d, file), { recursive: true, force: true }) });
const MKDIR = (dir) => ({ fs: (d) => fs.mkdirSync(path.join(d, dir), { recursive: true }) });
const TICK = { tick: true };
const G = (...a) => ({ git: a });
const GI = (input, ...a) => ({ git: a, input });
const GE = (env, ...a) => ({ git: a, env });
const GC = (cwd, ...a) => ({ git: a, cwd });
const ERR = (...a) => ({ git: a, err: true });
const REAL_ONLY = (...a) => ({ git: a, realOnly: true });   // setup done with real git on both sides
const SH = (fn) => ({ fs: fn });

const INIT = [G('init', '-q', '-b', 'main'), G('config', 'user.name', 'Repo User'), G('config', 'user.email', 'repo@example.com')];
const BASE = [...INIT, W('a.txt', 'one\ntwo\nthree\n'), W('dir/b.txt', 'bee\n'), G('add', '-A'), G('commit', '-q', '-m', 'first')];
const lines = (n, f = (i) => `line ${i}`) => Array.from({ length: n }, (_, i) => f(i + 1)).join('\n') + '\n';

// ---------------------------------------------------------------------------
// cases
// ---------------------------------------------------------------------------
const cases = [];
const C = (name, steps, opts = {}) => cases.push({ name, steps, ...opts });

// --- init, config ---
C('init default', [G('init')]);
C('init -b and reinit', [G('init', '-b', 'trunk'), G('init'), G('symbolic-ref', 'HEAD')]);
C('init bare', [G('init', '--bare', 'srv.git'), GC('srv.git', 'rev-parse', '--is-bare-repository')]);
C('config set get list', [...INIT, G('config', 'a.b', 'c d'), G('config', 'a.b'), G('config', '--get', 'a.b'), G('config', 'x.y.z', '1'),
  G('config', '--get', 'nope.nope'), G('config', '--bool', 'core.bare'), G('config', '--add', 'm.v', '1'), G('config', '--add', 'm.v', '2'),
  G('config', '--get-all', 'm.v'), G('config', 'm.v'), G('config', '--unset-all', 'm.v'), G('config', '--get-all', 'm.v'),
  G('config', '--unset', 'nope.x'), G('config', 'user.name')]);
C('config global', [G('config', '--global', 'user.name', 'Glob'), G('config', '--global', 'user.name'), ...INIT, G('config', 'user.name')]);
C('config -c override', [...INIT, G('-c', 'user.name=Over', 'config', 'user.name'), G('-c', 'core.bare', 'config', '--bool', 'core.bare')]);
C('config section ops', [...INIT, G('config', 'q.a', '1'), G('config', 'q.b', '2'), G('config', '--rename-section', 'q', 'r'), G('config', 'r.a'),
  G('config', '--remove-section', 'r'), G('config', 'r.a')]);
C('config value quoting', [...INIT, G('config', 'k.v', ' lead and # hash '), G('config', 'k.v'), G('config', 'k.w', 'a"b\\c'), G('config', 'k.w')]);

// --- status ---
C('status empty repo', [...INIT, G('status'), G('status', '--porcelain'), G('status', '-s', '-b')]);
C('status untracked collapsed', [...INIT, W('x', '1'), W('d/e/f', '2'), W('d/g', '3'), G('status', '--porcelain'),
  G('status', '--porcelain', '--untracked-files=all'), G('status', '--porcelain', '-uno'), G('status')]);
C('status after changes', [...BASE, W('a.txt', 'one\nTWO\nthree\n'), W('new.txt', 'n\n'), RM('dir/b.txt'), G('status', '--porcelain'), G('status'),
  G('add', 'new.txt'), G('status', '--porcelain'), G('status', '-s'), G('status')]);
C('status rename staged', [...BASE, G('mv', 'a.txt', 'renamed.txt'), G('status', '--porcelain'), G('status')]);
C('status gitignore', [...INIT, W('.gitignore', 'node_modules/\n*.log\n!keep.log\nbuild\n/root-only\n'), W('node_modules/x/y.js', 'x'), W('a.log', 'l'),
  W('keep.log', 'k'), W('build/out', 'o'), W('sub/build', 'b'), W('root-only', 'r'), W('sub/root-only', 's'), W('src/main.c', 'c'),
  G('status', '--porcelain'), G('status', '--porcelain', '-uall'), G('add', '-A'), G('status', '--porcelain'), G('ls-files')]);
C('status nested gitignore', [...INIT, W('.gitignore', '*.tmp\n'), W('a/.gitignore', '!keep.tmp\nlocal\n'), W('a/keep.tmp', '1'), W('a/x.tmp', '2'),
  W('a/local', '3'), W('b/local', '4'), W('b/y.tmp', '5'), G('status', '--porcelain', '-uall')]);
C('status gitignore globs', [...INIT, W('.gitignore', '**/gen/\nfoo/**/bar\n*.o\ndoc/*.txt\n[Tt]emp*\n\\#hash\n'), W('x/gen/a', '1'), W('gen/b', '2'),
  W('foo/bar', '3'), W('foo/a/b/bar', '4'), W('m.o', '5'), W('doc/a.txt', '6'), W('doc/sub/b.txt', '7'), W('Temp1', '8'), W('temp2', '9'),
  W('#hash', '10'), W('keep', '11'), G('status', '--porcelain', '-uall')]);
C('status in subdir', [...BASE, W('dir/c.txt', 'c'), W('top.txt', 't'), W('a.txt', 'changed\n'), GC('dir', 'status', '-s'), GC('dir', 'status', '--porcelain'), GC('dir', 'status')]);
C('status tracking ahead/behind', [...BASE, G('branch', 'other'), W('a.txt', 'x\n'), G('commit', '-qam', 'two'),
  G('config', 'branch.main.remote', '.'), G('config', 'branch.main.merge', 'refs/heads/other'), G('status'), G('status', '-sb'), G('branch', '-vv')]);
C('status quoted paths', [...INIT, W('sp ace.txt', '1'), ...(process.platform === 'win32' ? [] : [W('tab\there', '2'), W('quo"te', '3')]), W('ünï.txt', '4'), G('status', '--porcelain'),
  G('status', '--porcelain', '-z'), G('add', '-A'), G('status', '--porcelain'), G('ls-files'), G('ls-files', '-z')]);

// --- add, rm, mv ---
C('add pathspecs', [...BASE, W('a.txt', 'A\n'), W('dir/b.txt', 'B\n'), W('dir/n.txt', 'n\n'), W('top.md', 't'), G('add', 'dir'), G('status', '--porcelain'),
  G('add', '*.md'), G('status', '--porcelain'), G('add', '.'), G('status', '--porcelain')]);
C('add -u and removal', [...BASE, RM('a.txt'), W('dir/b.txt', 'x\n'), W('u.txt', 'u'), G('add', '-u'), G('status', '--porcelain')]);
C('add ignored needs -f', [...INIT, W('.gitignore', '*.log\n'), W('x.log', 'l'), ERR('add', 'x.log'), G('add', '-f', 'x.log'), G('status', '--porcelain')]);
C('add missing pathspec', [...INIT, ERR('add', 'nope')]);
C('add dry run', [...BASE, W('n.txt', 'n'), RM('a.txt'), G('add', '-n', '-A'), G('status', '--porcelain')]);
C('rm', [...BASE, G('rm', 'a.txt'), G('status', '--porcelain'), ERR('rm', 'dir'), G('rm', '-r', '-q', 'dir'), G('status', '--porcelain')]);
C('rm cached', [...BASE, G('rm', '--cached', 'a.txt'), G('status', '--porcelain')]);
C('rm modified refuses', [...BASE, W('a.txt', 'changed\n'), ERR('rm', 'a.txt'), G('rm', '-f', 'a.txt')]);
C('mv', [...BASE, G('mv', 'a.txt', 'z.txt'), G('mv', 'dir', 'newdir'), G('status', '--porcelain')]);

// --- commit ---
C('commit forms', [...INIT, W('a', '1\n'), G('add', 'a'), G('commit', '-m', 'one'), W('a', '2\n'), G('commit', '-am', 'two'),
  W('b', 'b\n'), G('add', 'b'), G('commit', '-q', '-m', 'three', '-m', 'body para'), G('log', '--format=%H %s|%b|')]);
C('commit nothing to commit', [...BASE, G('commit', '-m', 'nothing')]);
C('commit nothing in empty repo', [...INIT, G('commit', '-m', 'x')]);
C('commit untracked only', [...BASE, W('u', 'u'), G('commit', '-m', 'x')]);
C('commit allow-empty and amend', [...BASE, G('commit', '--allow-empty', '-m', 'empty'), TICK, G('commit', '--amend', '-m', 'amended'),
  G('log', '--format=%H %P %s')]);
C('commit amend no-edit keeps message', [...BASE, W('a.txt', 'x\n'), G('add', '-A'), TICK, G('commit', '--amend', '--no-edit'), G('log', '--format=%s %ad %cd')]);
C('commit -F and stdin', [...BASE, W('m.txt', 'from file\n\nwith body\n'), W('a.txt', 'x\n'), G('commit', '-a', '-F', 'm.txt'),
  W('a.txt', 'y\n'), GI('stdin message\n', 'commit', '-a', '-F', '-'), G('log', '--format=%B---')]);
C('commit message cleanup', [...BASE, W('a.txt', 'x\n'), G('commit', '-a', '-m', '\n\n  title  \n\n\n\nbody   \n\n# not a comment\n\n'), G('log', '-1', '--format=%B|')]);
C('commit author option', [...BASE, W('a.txt', 'x\n'), G('commit', '-a', '-m', 'm', '--author', 'Other Person <o@p.q>'), G('log', '-1', '--format=%an|%ae|%cn|%ce')]);
C('commit date option', [...BASE, W('a.txt', 'x\n'), G('commit', '-a', '-m', 'm', '--date', '2020-01-02T03:04:05+0100'), G('log', '-1', '--format=%ad|%aI')]);
C('commit signoff', [...BASE, W('a.txt', 'x\n'), G('commit', '-a', '-s', '-m', 'signed'), G('log', '-1', '--format=%B')]);
C('commit paths only', [...BASE, W('a.txt', 'A\n'), W('dir/b.txt', 'B\n'), G('commit', '-m', 'only a', 'a.txt'), G('status', '--porcelain'), G('show', '--stat', '--format=%s')]);
C('commit with unmerged refuses', [...BASE, G('checkout', '-q', '-b', 'side'), W('a.txt', 'side\n'), G('commit', '-qam', 's'), G('checkout', '-q', 'main'),
  W('a.txt', 'main\n'), G('commit', '-qam', 'm'), G('merge', 'side'), ERR('commit', '-m', 'x')]);
C('commit identity from config', [...INIT, W('a', '1'), G('add', 'a'), GE({ GIT_AUTHOR_NAME: '', GIT_AUTHOR_EMAIL: '', GIT_COMMITTER_NAME: '', GIT_COMMITTER_EMAIL: '' }, 'commit', '-q', '-m', 'cfg'),
  G('log', '--format=%an %ae %cn %ce')]);
C('commit exec bit kept', [...INIT, W('s.sh', '#!/bin/sh\n'), G('add', 's.sh'), G('update-index', '--chmod=+x', 's.sh'), G('commit', '-qm', 'x'),
  W('s.sh', '#!/bin/sh\necho\n'), G('add', '-A'), G('commit', '-qm', 'y'), G('ls-tree', 'HEAD'), G('ls-files', '-s')]);

// --- log, show, rev-list, rev-parse ---
const HIST = [...BASE, TICK, W('a.txt', 'one\n2\nthree\n'), G('commit', '-qam', 'second\n\nbody of second'), TICK, G('checkout', '-qb', 'feat'),
  W('f.txt', 'f\n'), G('add', 'f.txt'), G('commit', '-qm', 'feature'), TICK, G('checkout', '-q', 'main'), W('dir/b.txt', 'bee2\n'),
  G('commit', '-qam', 'main work'), TICK, G('merge', '-q', '--no-edit', 'feat'), G('tag', 'v1'), G('tag', '-a', 'v2', '-m', 'annotated', 'HEAD~1')];
C('log default and oneline', [...HIST, G('log'), G('log', '--oneline'), G('log', '-2'), G('log', '-n', '1', '--format=%H'), G('log', '--max-count=3', '--oneline')]);
C('log formats', [...HIST, G('log', '--format=%H|%h|%T|%t|%P|%p|%an|%ae|%ad|%at|%aI|%as|%cn|%ce|%cd|%ct|%cI|%s|%f'), G('log', '--pretty=format:%h %s'),
  G('log', '--pretty=oneline'), G('log', '--pretty=short'), G('log', '--pretty=full'), G('log', '--pretty=raw', '-1'), G('log', '--format=%b|%B|', '-2'),
  G('log', '--date=iso', '-1'), G('log', '--date=short', '--format=%ad', '-1'), G('log', '--date=unix', '--format=%ad', '-1'), G('log', '--date=raw', '--format=%ad', '-1'),
  G('log', '--date=rfc', '--format=%ad', '-1'), G('log', '--date=iso-strict', '--format=%ad|%cd', '-1')]);
C('log ranges', [...HIST, G('log', '--oneline', 'main..feat'), G('log', '--oneline', 'feat..main'), G('log', '--oneline', 'feat...main'), G('log', '--oneline', '^feat', 'main'),
  G('log', '--oneline', 'HEAD~1'), G('log', '--oneline', '--first-parent'), G('log', '--oneline', '--no-merges'), G('log', '--oneline', '--merges'),
  G('log', '--oneline', '--reverse'), G('log', '--oneline', '--all'), G('log', '--oneline', '--skip=1', '-2'), G('log', '--oneline', '--author=Thor'),
  G('log', '--oneline', '--grep=feat'), G('log', '--oneline', '-i', '--grep=MAIN')]);
C('log paths', [...HIST, G('log', '--oneline', '--', 'a.txt'), G('log', '--oneline', 'dir'), G('log', '--oneline', '--', 'f.txt'), G('log', '--format=%s', '--', 'nope')]);
C('log with diffs', [...HIST, G('log', '--stat'), G('log', '-p', '-2'), G('log', '--name-only', '--format=%h'), G('log', '--name-status', '--oneline'),
  G('log', '--numstat', '--format=%s'), G('log', '--oneline', '--stat', '-3'), G('log', '--shortstat', '--oneline')]);
C('log decorate', [...HIST, G('log', '--oneline', '--decorate'), G('log', '--format=%d|%D'), G('branch', 'x', 'HEAD~2'), G('log', '--oneline', '--decorate', '--all')]);
C('log empty repo', [...INIT, ERR('log')]);
C('show', [...HIST, G('show'), G('show', 'HEAD~1'), G('show', '--stat', 'HEAD~1'), G('show', '--name-only', '--format=', 'HEAD~1'), G('show', '--numstat', '--format=', 'HEAD~2'),
  G('show', 'HEAD:a.txt'), G('show', 'v2'), G('show', 'HEAD^{tree}'), G('show', '-s', '--format=%s', 'HEAD'), G('show', '--oneline', '-s'), G('show', 'HEAD~1:dir')]);
C('rev-list', [...HIST, G('rev-list', 'HEAD'), G('rev-list', '--count', 'HEAD'), G('rev-list', '--count', 'feat..main'), G('rev-list', '--first-parent', 'HEAD'),
  G('rev-list', '--max-parents=0', 'HEAD'), G('rev-list', '--reverse', 'HEAD'), G('rev-list', '-n', '2', 'HEAD'), G('rev-list', '--all'), G('rev-list', 'main', '--not', 'feat'),
  G('rev-list', '--count', 'HEAD..HEAD'), G('rev-list', '--no-merges', '--count', 'HEAD'), G('rev-list', 'HEAD', '--', 'dir')]);
C('rev-parse', [...HIST, G('rev-parse', 'HEAD'), G('rev-parse', '--short', 'HEAD'), G('rev-parse', '--abbrev-ref', 'HEAD'), G('rev-parse', 'HEAD~1', 'HEAD^2', 'HEAD^{tree}'),
  G('rev-parse', 'HEAD:a.txt'), G('rev-parse', 'v2', 'v2^{}', 'v2^{commit}'), G('rev-parse', '--verify', 'main'), G('rev-parse', '--verify', '-q', 'nope'),
  ERR('rev-parse', '--verify', 'nope'), ERR('rev-parse', 'nope'), G('rev-parse', '--show-toplevel'), G('rev-parse', '--git-dir'), G('rev-parse', '--is-inside-work-tree'),
  G('rev-parse', '--is-bare-repository'), G('rev-parse', '--symbolic-full-name', 'HEAD'), G('rev-parse', 'main..feat'), G('rev-parse', '--show-prefix'),
  GC('dir', 'rev-parse', '--show-prefix'), GC('dir', 'rev-parse', '--show-cdup'), GC('dir', 'rev-parse', '--git-dir'), G('rev-parse', '--git-common-dir'),
  G('rev-parse', 'HEAD@{0}'), G('rev-parse', ':a.txt'), G('rev-parse', '--abbrev-ref', 'feat'), G('rev-parse', '--short=10', 'HEAD'), G('rev-parse', 'refs/heads/feat')]);
C('rev-parse upstream', [...BASE, G('branch', 'up'), G('branch', '--set-upstream-to=up'), G('rev-parse', '--abbrev-ref', '@{u}'), G('rev-parse', '--symbolic-full-name', '@{upstream}'),
  G('rev-parse', 'main@{u}')]);
C('rev-parse outside repo', [ERR('rev-parse', '--is-inside-work-tree'), ERR('status')]);
C('merge-base', [...HIST, G('merge-base', 'main', 'feat'), G('merge-base', '--is-ancestor', 'feat', 'main'), G('merge-base', '--is-ancestor', 'main', 'feat'),
  G('merge-base', '--all', 'HEAD~1', 'feat')]);

// --- diff ---
const DIFFBASE = [...BASE, W('big.txt', lines(40)), W('bin.dat', Buffer.from([0, 1, 2, 3, 0, 255])), W('noeol.txt', 'no newline'), G('add', '-A'), G('commit', '-qm', 'more')];
C('diff worktree', [...DIFFBASE, W('a.txt', 'one\nTWO\nthree\nfour\n'), W('big.txt', lines(40, (i) => (i === 5 || i === 30 ? `changed ${i}` : `line ${i}`))),
  W('noeol.txt', 'no newline still'), RM('dir/b.txt'), G('diff'), G('diff', '--stat'), G('diff', '--numstat'), G('diff', '--name-only'), G('diff', '--name-status'),
  G('diff', '--shortstat'), G('diff', '-U1'), G('diff', '-U0'), G('diff', '--raw'), G('diff', '--', 'a.txt'), G('diff', '--exit-code'), G('diff', '--quiet'), G('diff', '--summary')]);
C('diff cached and revs', [...DIFFBASE, W('a.txt', 'x\n'), W('new.txt', 'new\n'), G('add', '-A'), G('diff', '--cached'), G('diff', '--staged', '--stat'),
  G('diff', '--cached', '--quiet'), G('commit', '-qm', 'c'), G('diff', 'HEAD~1'), G('diff', 'HEAD~1', 'HEAD'), G('diff', 'HEAD~1..HEAD', '--numstat'),
  G('diff', 'HEAD~2...HEAD', '--name-only'), G('diff', '--no-renames', '--numstat', 'HEAD~1', 'HEAD'), G('diff', '--name-only', '--no-renames', '-z', 'HEAD~2', 'HEAD')]);
C('diff binary', [...DIFFBASE, W('bin.dat', Buffer.from([0, 9, 9, 9])), G('diff'), G('diff', '--stat'), G('diff', '--numstat'), G('add', '-A'), G('diff', '--cached', '--binary')]);
C('diff renames', [...DIFFBASE, G('mv', 'big.txt', 'moved.txt'), W('moved.txt', lines(40, (i) => (i === 3 ? 'x' : `line ${i}`))), G('add', '-A'),
  G('diff', '--cached'), G('diff', '--cached', '--stat'), G('diff', '--cached', '--numstat'), G('diff', '--cached', '--name-status'), G('diff', '--cached', '-M90%', '--name-status'),
  G('diff', '--cached', '--no-renames', '--name-status'), G('mv', 'dir/b.txt', 'dir/c.txt'), G('diff', '--cached', '--stat')]);
C('diff mode change', [...DIFFBASE, G('update-index', '--chmod=+x', 'a.txt'), G('diff', '--cached'), G('diff', '--cached', '--summary'), G('diff', '--cached', '--raw')]);
C('diff filter', [...DIFFBASE, W('a.txt', 'x\n'), RM('dir/b.txt'), W('n', 'n\n'), G('add', '-A'), G('diff', '--cached', '--name-only', '--diff-filter=AD'),
  G('diff', '--cached', '--name-only', '--diff-filter=d'), G('diff', '--cached', '--name-only', '--diff-filter=M')]);
C('diff no newline at eof', [...DIFFBASE, W('noeol.txt', 'no newline\n'), G('diff'), W('a.txt', 'one\ntwo\nthree'), G('diff', '--', 'a.txt')]);
C('diff empty files', [...BASE, W('empty', ''), G('add', 'empty'), G('diff', '--cached'), G('commit', '-qm', 'e'), W('empty', 'now\n'), G('diff'), RM('empty'), G('diff')]);
C('diff unmerged', [...BASE, G('checkout', '-qb', 's'), W('a.txt', 's\n'), G('commit', '-qam', 's'), G('checkout', '-q', 'main'), W('a.txt', 'm\n'), G('commit', '-qam', 'm'),
  G('merge', 's'), G('diff', '--name-only', '--diff-filter=U'), G('diff', '--name-status'), G('diff', '--cached', '--name-only')]);
C('diff --no-index', [W('x', '1\n2\n'), W('y', '1\n3\n'), G('diff', '--no-index', 'x', 'y'), G('diff', '--no-index', '--numstat', 'x', 'y')]);

// --- branch, checkout, switch, tag ---
C('branch ops', [...HIST, G('branch'), G('branch', '-a'), G('branch', '-v'), G('branch', 'new'), G('branch', 'new2', 'feat'), ERR('branch', 'new'),
  G('branch', '-f', 'new', 'HEAD~2'), G('branch', '-m', 'new', 'renamed'), G('branch', '--list', 're*'), G('branch', '-d', 'renamed'), ERR('branch', '-d', 'nope'),
  G('branch', '--show-current'), G('branch', '--merged'), G('branch', '--no-merged', 'feat'), G('branch', '--contains', 'feat'), G('branch', '-D', 'new2'),
  G('branch', '--format=%(refname:short) %(objectname:short)')]);
C('branch delete unmerged', [...HIST, G('checkout', '-qb', 'tmp'), W('t', 't'), G('add', 't'), G('commit', '-qm', 't'), G('checkout', '-q', 'main'),
  ERR('branch', '-d', 'tmp'), G('branch', '-D', 'tmp')]);
C('checkout branches', [...HIST, ERR('checkout', 'feat'), G('status', '-sb'), ERR('checkout', '-b', 'nb'), ERR('checkout', 'main'), ERR('checkout', '-'), G('branch', '--show-current'),
  ERR('checkout', '-B', 'nb', 'HEAD~2'), G('log', '--oneline', '-1')]);
C('checkout detached', [...HIST, ERR('checkout', 'HEAD~1'), G('status'), G('branch'), ERR('checkout', '--detach', 'main'), ERR('checkout', 'main')]);
C('checkout carries changes', [...HIST, W('a.txt', 'local\n'), ERR('checkout', 'feat'), G('status', '--porcelain'), G('checkout', '-q', 'main')]);
C('checkout refuses overwrite', [...HIST, W('f.txt', 'mine\n'), ERR('checkout', 'feat'), W('dir/b.txt', 'dirty\n'), ERR('checkout', 'HEAD~2'), G('status', '--porcelain')]);
C('checkout paths', [...HIST, W('a.txt', 'dirty\n'), W('dir/b.txt', 'dirty\n'), G('checkout', '--', 'a.txt'), G('status', '--porcelain'), G('checkout', 'HEAD~3', '--', 'dir/b.txt'),
  G('status', '--porcelain'), G('checkout', '.'), G('status', '--porcelain'), ERR('checkout', '--', 'nope')]);
C('checkout force', [...HIST, W('a.txt', 'dirty\n'), W('f.txt', 'mine'), ERR('checkout', '-f', 'HEAD~3'), G('status', '--porcelain')]);
C('checkout remote dwim', [...BASE, G('update-ref', 'refs/remotes/origin/rb', 'HEAD'), G('config', 'remote.origin.url', 'x'), G('config', 'remote.origin.fetch', '+refs/heads/*:refs/remotes/origin/*'),
  ERR('checkout', 'rb'), G('config', '--get-regexp', 'branch'), G('status', '-sb')]);
C('switch', [...HIST, ERR('switch', 'feat'), ERR('switch', '-c', 'sw'), ERR('switch', 'main'), ERR('switch', 'HEAD~1'), ERR('switch', '--detach', 'HEAD~1'), ERR('switch', '-')]);
C('tag', [...HIST, G('tag'), G('tag', '-l', 'v*'), G('tag', 'light', 'HEAD~2'), G('tag', '-a', 'ann', '-m', 'message here'), G('cat-file', '-p', 'ann'), ERR('tag', 'light'),
  G('tag', '-f', 'light'), G('tag', '-d', 'light'), G('tag', '--contains', 'HEAD~1'), G('tag', '--points-at', 'HEAD')]);

// --- restore, reset, clean, stash ---
C('restore', [...HIST, W('a.txt', 'dirty\n'), G('add', 'a.txt'), W('a.txt', 'dirtier\n'), G('restore', '--staged', 'a.txt'), G('status', '--porcelain'),
  G('restore', 'a.txt'), G('status', '--porcelain'), G('restore', '--source=HEAD~3', 'a.txt'), G('status', '--porcelain'), ERR('restore', 'nope'), G('restore', '-SW', '.'), G('status', '--porcelain')]);
C('reset modes', [...HIST, W('a.txt', 'dirty\n'), G('add', 'a.txt'), G('reset'), G('status', '--porcelain'), G('reset', '--soft', 'HEAD~1'), G('status', '--porcelain'),
  G('reset', 'HEAD~1'), G('status', '--porcelain'), G('reset', '--hard', 'HEAD'), G('status', '--porcelain'), G('reset', '--hard', 'feat'), G('log', '--oneline'), G('rev-parse', 'ORIG_HEAD')]);
C('reset paths', [...HIST, W('a.txt', 'x\n'), W('dir/b.txt', 'y\n'), G('add', '-A'), G('reset', '-q', '--', 'a.txt'), G('status', '--porcelain'), G('reset', 'HEAD~3', 'dir/b.txt'),
  G('status', '--porcelain'), G('diff', '--cached', '--stat')]);
C('clean', [...BASE, W('.gitignore', '*.log\n'), W('u.txt', 'u'), W('ud/x', 'x'), W('i.log', 'i'), W('dir/u2', 'u'), ERR('clean'), G('clean', '-n'), G('clean', '-nd'),
  G('clean', '-ndx'), G('clean', '-ndX'), G('clean', '-f'), G('status', '--porcelain', '--ignored'.replace('--ignored', '-uall')), G('clean', '-fd'), G('clean', '-ffdx'), G('status', '--porcelain')]);
C('stash', [...HIST, W('a.txt', 'stashed change\n'), W('dir/b.txt', 'staged\n'), G('add', 'dir/b.txt'), G('stash'), G('status', '--porcelain'), G('stash', 'list'),
  G('stash', 'show'), G('stash', 'show', '-p'), G('stash', 'pop'), G('status', '--porcelain'), G('stash', 'list'), G('stash', 'push', '-m', 'named'), G('stash', 'list'),
  G('stash', 'apply'), G('stash', 'drop'), G('stash', 'list'), G('stash')]);
C('stash untracked and keep-index', [...HIST, W('new.txt', 'n\n'), W('a.txt', 'x\n'), G('stash', '-u'), G('status', '--porcelain'), G('stash', 'pop'), G('status', '--porcelain'),
  G('add', 'a.txt'), W('dir/b.txt', 'unstaged\n'), G('stash', '--keep-index'), G('status', '--porcelain'), G('stash', 'pop', '--index'), G('status', '--porcelain')]);

// --- merge, cherry-pick, revert ---
const FORK = [...BASE, W('big.txt', lines(30)), G('add', '-A'), G('commit', '-qm', 'base'), TICK, G('checkout', '-qb', 'side'),
  W('big.txt', lines(30, (i) => (i === 3 ? 'side 3' : `line ${i}`))), W('side.txt', 's\n'), G('add', '-A'), G('commit', '-qm', 'side work'), TICK, G('checkout', '-q', 'main'),
  W('big.txt', lines(30, (i) => (i === 25 ? 'main 25' : `line ${i}`))), G('commit', '-qam', 'main work'), TICK];
C('merge clean', [...FORK, G('merge', '--no-edit', 'side'), G('log', '--format=%s %P'), G('show', '--format=%s', '--stat', 'HEAD'), G('status', '--porcelain')]);
C('merge into other branch message', [...FORK, G('checkout', '-qb', 'other'), G('merge', '--no-edit', 'side'), G('log', '-1', '--format=%s')]);
C('merge fast-forward', [...BASE, G('checkout', '-qb', 'ff'), W('ff.txt', 'f\n'), G('add', '-A'), G('commit', '-qm', 'ff'), G('checkout', '-q', 'main'), G('merge', 'ff'),
  G('merge', 'ff'), G('log', '--oneline'), G('reflog', '-1').git ? G('rev-parse', 'ORIG_HEAD') : G('rev-parse', 'ORIG_HEAD')]);
C('merge ff-only refuses', [...FORK, ERR('merge', '--ff-only', 'side'), G('status', '--porcelain')]);
C('merge no-ff', [...BASE, G('checkout', '-qb', 'ff'), W('ff.txt', 'f\n'), G('add', '-A'), G('commit', '-qm', 'ff'), G('checkout', '-q', 'main'), TICK,
  G('merge', '--no-ff', '--no-edit', 'ff'), G('log', '--format=%s %P'), G('merge', '--no-ff', '-m', 'custom', 'ff')]);
C('merge conflict and resolve', [...BASE, G('checkout', '-qb', 's'), W('a.txt', 'one\nside\nthree\n'), G('commit', '-qam', 's'), G('checkout', '-q', 'main'),
  W('a.txt', 'one\nmain\nthree\n'), G('commit', '-qam', 'm'), TICK, G('merge', 's'), G('status'), G('status', '--porcelain'), G('ls-files', '-s'), G('ls-files', '-u'),
  SH((d) => fs.copyFileSync(path.join(d, 'a.txt'), path.join(d, 'marker.out'))), G('diff', '--name-only', '--diff-filter=U'), W('a.txt', 'one\nresolved\nthree\n'),
  RM('marker.out'), G('add', 'a.txt'), G('status'), G('commit', '--no-edit'), G('log', '-1', '--format=%B|%P')], { files: true });
C('merge conflict abort', [...BASE, G('checkout', '-qb', 's'), W('a.txt', 'side\n'), G('commit', '-qam', 's'), G('checkout', '-q', 'main'), W('a.txt', 'main\n'), G('commit', '-qam', 'm'),
  W('dir/b.txt', 'unrelated local\n'), G('merge', 's'), G('merge', '--abort'), G('status', '--porcelain'), ERR('merge', '--abort')]);
C('merge modify/delete', [...BASE, G('checkout', '-qb', 's'), G('rm', '-q', 'a.txt'), G('commit', '-qm', 'del'), G('checkout', '-q', 'main'), W('a.txt', 'changed\n'),
  G('commit', '-qam', 'mod'), G('merge', 's'), G('status', '--porcelain'), G('ls-files', '-s')]);
C('merge add/add', [...BASE, G('checkout', '-qb', 's'), W('n.txt', 'side\n'), G('add', 'n.txt'), G('commit', '-qm', 'add s'), G('checkout', '-q', 'main'), W('n.txt', 'main\n'),
  G('add', 'n.txt'), G('commit', '-qm', 'add m'), G('merge', 's'), G('status', '--porcelain')], { files: true });
C('merge refuses dirty', [...FORK, W('big.txt', 'dirty\n'), ERR('merge', 'side'), G('status', '--porcelain')]);
C('merge unrelated', [...BASE, G('checkout', '-q', '--orphan', 'o'), G('rm', '-rqf', '.'), W('o.txt', 'o\n'), G('add', 'o.txt'), G('commit', '-qm', 'orphan'),
  G('checkout', '-q', 'main'), ERR('merge', 'o'), G('merge', '--allow-unrelated-histories', '--no-edit', 'o'), G('ls-files')]);
C('merge FETCH_HEAD', [...FORK, SH((d) => { const id = spawnSync(REAL, ['rev-parse', 'side'], { cwd: d, encoding: 'utf8' }).stdout.trim(); fs.writeFileSync(path.join(d, '.git', 'FETCH_HEAD'), `${id}\t\tbranch 'main' of https://example.com/r.git\n`); }),
  G('merge', '--no-edit', 'FETCH_HEAD'), G('log', '-1', '--format=%s')]);
C('cherry-pick', [...FORK, G('cherry-pick', 'side'), G('log', '--format=%s|%an|%ad'), G('cherry-pick', '-x', 'side~1').git ? G('status', '--porcelain') : G('status')]);
C('cherry-pick conflict', [...BASE, G('checkout', '-qb', 's'), W('a.txt', 'side\n'), G('commit', '-qam', 's'), G('checkout', '-q', 'main'), W('a.txt', 'main\n'), G('commit', '-qam', 'm'),
  ERR('cherry-pick', 's'), G('status', '--porcelain'), G('cherry-pick', '--abort'), G('status', '--porcelain'), G('log', '--oneline')]);
C('revert', [...FORK, G('revert', '--no-edit', 'HEAD'), G('log', '--format=%s%n%b'), G('status', '--porcelain')]);

// --- plumbing ---
C('cat-file', [...HIST, G('cat-file', '-t', 'HEAD'), G('cat-file', '-s', 'HEAD:a.txt'), G('cat-file', '-p', 'HEAD'), G('cat-file', '-p', 'HEAD^{tree}'), G('cat-file', 'blob', 'HEAD:a.txt'),
  G('cat-file', 'commit', 'HEAD'), G('cat-file', '-e', 'HEAD'), G('cat-file', '-e', 'HEAD:nope'), G('cat-file', '-p', 'v2'), GI('HEAD\nHEAD:a.txt\nnope\n', 'cat-file', '--batch-check'),
  GI('HEAD:a.txt\n', 'cat-file', '--batch'), G('cat-file', '-e', 'HEAD^{commit}'), G('cat-file', '-e', 'HEAD:dir/b.txt')]);
C('hash-object mktree', [...INIT, W('f', 'content\n'), G('hash-object', 'f'), G('hash-object', '-w', 'f'), G('hash-object', '-t', 'tree', process.platform === 'win32' ? 'NUL' : '/dev/null'),
  GI('stdin content', 'hash-object', '--stdin'), GI('100644 blob d95f3ad14dee633a758d2e331151e950dd13e4ed\tf\n', 'mktree'), G('cat-file', '-p', 'b9df3e5c0d2c37ac0eb1d8d0e5c3caef7bff2fd2')]);
C('index plumbing like converge', [...HIST, GE({ GIT_INDEX_FILE: '.git/tmp.idx' }, 'read-tree', 'HEAD~2'), GE({ GIT_INDEX_FILE: '.git/tmp.idx' }, 'write-tree'),
  SH((d) => {
    const out = spawnSync(REAL, ['ls-tree', '-r', '--full-tree', 'feat'], { cwd: d, encoding: 'utf8' }).stdout;
    fs.writeFileSync(path.join(d, '.git', 'lstree.out'), out);
  }),
  { git: ['update-index', '--add', '--index-info'], env: { GIT_INDEX_FILE: '.git/tmp.idx' }, inputFile: '.git/lstree.out' },
  GE({ GIT_INDEX_FILE: '.git/tmp.idx' }, 'write-tree'), GE({ GIT_INDEX_FILE: '.git/tmp.idx' }, 'ls-files', '-s'),
  G('commit-tree', 'HEAD^{tree}', '-p', 'HEAD', '-p', 'feat', '-m', 'synthetic'), G('commit-tree', 'HEAD^{tree}', '-m', 'one', '-m', 'two'),
  GI('from stdin\n', 'commit-tree', 'HEAD^{tree}')]);
C('read-tree prefix and empty', [...HIST, GE({ GIT_INDEX_FILE: '.git/i2' }, 'read-tree', '--empty'), GE({ GIT_INDEX_FILE: '.git/i2' }, 'read-tree', '--prefix=sub/', 'HEAD'),
  GE({ GIT_INDEX_FILE: '.git/i2' }, 'read-tree', '--prefix=other/', 'feat'), GE({ GIT_INDEX_FILE: '.git/i2' }, 'write-tree'), GE({ GIT_INDEX_FILE: '.git/i2' }, 'ls-files'),
  { git: ['read-tree', '--prefix=sub/', 'HEAD'], env: { GIT_INDEX_FILE: '.git/i2' }, err: true }]);
C('read-tree reset -u', [...HIST, W('a.txt', 'dirty\n'), G('read-tree', '--reset', '-u', 'HEAD~3'), G('status', '--porcelain'), G('read-tree', '-u', '--reset', 'HEAD')]);
C('update-index', [...BASE, W('a.txt', 'x\n'), G('update-index', 'a.txt'), G('status', '--porcelain'), W('n', 'n'), ERR('update-index', 'n'), G('update-index', '--add', 'n'),
  RM('n'), ERR('update-index', 'n'), G('update-index', '--remove', 'n'), G('update-index', '--cacheinfo', '100644,d95f3ad14dee633a758d2e331151e950dd13e4ed,c'),
  G('update-index', '--add', '--cacheinfo', '100644,d95f3ad14dee633a758d2e331151e950dd13e4ed,c'), G('ls-files', '-s'), G('update-index', '--skip-worktree', 'dir/b.txt'),
  W('dir/b.txt', 'hidden change\n'), G('status', '--porcelain'), G('update-index', '--no-skip-worktree', 'dir/b.txt'), G('status', '--porcelain'),
  G('update-index', '--assume-unchanged', 'dir/b.txt'), G('status', '--porcelain'), G('ls-files', '-v').git ? G('status') : G('status')]);
C('update-ref', [...HIST, G('update-ref', 'refs/x/one', 'HEAD'), G('show-ref'), G('update-ref', 'refs/x/one', 'HEAD~1', 'HEAD'), ERR('update-ref', 'refs/x/one', 'HEAD', 'HEAD'),
  ERR('update-ref', 'refs/x/one', 'HEAD', ''), G('update-ref', 'refs/x/two', 'HEAD', ''), G('update-ref', '-d', 'refs/x/two'), ERR('update-ref', '-d', 'refs/x/one', 'HEAD'),
  G('update-ref', '-d', 'refs/x/one', 'HEAD~1'), G('show-ref'), GI('create refs/y/a HEAD\nupdate refs/heads/feat HEAD~1\n', 'update-ref', '--stdin'), G('show-ref')]);
C('update-ref forge lock shape', [...BASE, W('lock.json', '{"id":1}\n'), G('hash-object', '-w', 'lock.json'),
  GI('100644 blob c8ba8dbc0b0b0e5f7ca59bd27a00e05f72d8b2c6\tlock\n', 'mktree', '--missing'), G('commit-tree', '-m', 'forge lock x by n at t', '4b825dc642cb6eb9a060e54bf8d69288fbee4904'),
  G('update-ref', 'refs/forge/a', 'HEAD', ''), ERR('update-ref', 'refs/forge/a', 'HEAD', ''), G('cat-file', 'commit', 'HEAD')]);
C('symbolic-ref', [...BASE, G('symbolic-ref', 'HEAD'), G('symbolic-ref', '--short', 'HEAD'), G('symbolic-ref', 'HEAD', 'refs/heads/other'), G('symbolic-ref', 'HEAD'),
  G('symbolic-ref', 'HEAD', 'refs/heads/main'), G('checkout', '-q', '--detach'), ERR('symbolic-ref', 'HEAD'), G('symbolic-ref', '-q', 'HEAD')]);
C('ls-files', [...HIST, W('.gitignore', 'ig*\n'), W('ig.txt', 'i'), W('u.txt', 'u'), W('ud/x', 'x'), G('ls-files'), G('ls-files', '-z'), G('ls-files', '-s'),
  G('ls-files', '--others'), G('ls-files', '--others', '--exclude-standard'), G('ls-files', '-o', '--exclude-standard', '--directory'), G('ls-files', '-z', '--cached', '--others', '--exclude-standard'),
  G('ls-files', '--', 'dir'), G('ls-files', '*.txt'), G('ls-files', 'dir/*'), GC('dir', 'ls-files'), GC('dir', 'ls-files', '--full-name'), ERR('ls-files', '--error-unmatch', 'nope'),
  W('a.txt', 'm'), RM('f.txt'), G('ls-files', '-m'), G('ls-files', '-d'), G('ls-files', '-i', '-o', '--exclude-standard')]);
C('ls-tree', [...HIST, G('ls-tree', 'HEAD'), G('ls-tree', '-r', 'HEAD'), G('ls-tree', '-r', '-t', 'HEAD'), G('ls-tree', '-d', 'HEAD'), G('ls-tree', '-l', 'HEAD'),
  G('ls-tree', '-r', '-l', 'HEAD'), G('ls-tree', '--name-only', '-r', 'HEAD'), G('ls-tree', 'HEAD', 'dir'), G('ls-tree', 'HEAD', 'dir/'), G('ls-tree', '-r', 'HEAD', 'dir'),
  G('ls-tree', '-r', '--full-tree', 'feat'), GC('dir', 'ls-tree', 'HEAD'), GC('dir', 'ls-tree', '--full-tree', 'HEAD'), G('ls-tree', '-z', 'HEAD'), G('ls-tree', 'HEAD:dir')]);
C('notes', [...HIST, G('notes', '--ref=converge', 'add', '-m', '{"a":1}', 'HEAD'), G('notes', '--ref=converge', 'show', 'HEAD'), G('notes', '--ref=converge', 'list'),
  ERR('notes', '--ref=converge', 'add', '-m', 'again', 'HEAD'), G('notes', '--ref=converge', 'add', '-f', '-m', 'again', 'HEAD'), G('notes', '--ref=converge', 'show', 'HEAD'),
  ERR('notes', '--ref=converge', 'show', 'HEAD~1'), G('notes', 'add', '-m', 'default ref'), G('log', '-1', '--format=%N'), G('notes', 'list')]);
C('show-ref for-each-ref', [...HIST, G('show-ref'), G('show-ref', '--heads'), G('show-ref', '--tags'), G('show-ref', '-d'), G('show-ref', '--verify', 'refs/heads/main'),
  ERR('show-ref', '--verify', 'refs/heads/nope'), G('for-each-ref'), G('for-each-ref', '--format=%(refname) %(objectname:short) %(objecttype)', 'refs/heads'),
  G('for-each-ref', '--format=%(refname:short)', 'refs/tags/'), G('for-each-ref', '--count=1', 'refs/heads')]);
C('check-ignore', [...INIT, W('.gitignore', '*.log\nbuild/\n'), W('a.log', 'l'), W('build/x', 'x'), G('check-ignore', 'a.log', 'b.txt', 'build/x'), G('check-ignore', 'nope.txt'),
  G('check-ignore', '-q', 'a.log')]);
C('archive tar', [...HIST, SH((d) => fs.mkdirSync(path.join(d, 'ext'))), { git: ['archive', 'HEAD'], tarTo: 'ext' }, { git: ['archive', '--prefix=p/', 'HEAD:dir'], tarTo: 'ext2' }], { files: true });
C('grep', [...HIST, G('grep', 'one'), G('grep', '-n', 'e'), G('grep', '-i', 'TWO'), G('grep', '-l', 'e'), G('grep', '-c', 'e'), G('grep', 'nope'), G('grep', '-E', 'b+e'),
  G('grep', '-F', '.'), G('grep', '-w', 'bee2'), G('grep', 'e', '--', 'dir'), G('grep', 'one', 'HEAD~3'), G('grep', '-v', 'e', '--', 'a.txt'), GC('dir', 'grep', 'bee')]);
C('apply patches', [...DIFFBASE, W('a.txt', 'one\nTWO\nthree\nfour\n'), W('big.txt', lines(40, (i) => (i === 7 ? 'seven' : `line ${i}`))), W('n.txt', 'new file\n'), RM('dir/b.txt'),
  G('add', '-N', 'n.txt').git ? G('add', '-A') : G('add', '-A'), SH((d) => {
    const patch = spawnSync(REAL, ['diff', '--cached', '--binary', 'HEAD'], { cwd: d, encoding: 'buffer' }).stdout;
    fs.writeFileSync(path.join(d, '..', path.basename(d) + '.patch'), patch);
  }), G('reset', '-q', '--hard'), { git: ['apply', '--check'], inputFile: '../{self}.patch' }, { git: ['apply'], inputFile: '../{self}.patch' }, G('status', '--porcelain'),
  G('reset', '-q', '--hard'), { git: ['apply', '--index'], inputFile: '../{self}.patch' }, G('status', '--porcelain'), { git: ['apply', '-R', '--index'], inputFile: '../{self}.patch' },
  G('status', '--porcelain'), { git: ['apply', '--cached'], inputFile: '../{self}.patch' }, G('status', '--porcelain')], { files: true });
C('apply binary', [...DIFFBASE, W('bin.dat', Buffer.from([7, 0, 7, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9])), W('newbin', Buffer.from([0, 0, 1])), G('add', '-A'), SH((d) => {
    const patch = spawnSync(REAL, ['diff', '--cached', '--binary', 'HEAD'], { cwd: d, encoding: 'buffer' }).stdout;
    fs.writeFileSync(path.join(d, '..', path.basename(d) + '.patch'), patch);
  }), G('reset', '-q', '--hard'), { git: ['apply', '--binary', '-'], inputFile: '../{self}.patch' }, G('status', '--porcelain')], { files: true });
C('apply refuses mismatch', [...BASE, SH((d) => fs.writeFileSync(path.join(d, '..', path.basename(d) + '.patch'),
  'diff --git a/a.txt b/a.txt\n--- a/a.txt\n+++ b/a.txt\n@@ -1,3 +1,3 @@\n one\n-NOT THERE\n+x\n three\n')), { git: ['apply'], inputFile: '../{self}.patch', err: true }]);
C('worktree', [...HIST, { git: ['worktree', 'add', '--detach', '../{self}-wt', 'HEAD~1'] }, { git: ['status', '--porcelain'], cwd: '../{self}-wt' },
  { git: ['rev-parse', 'HEAD'], cwd: '../{self}-wt' }, { fsWt: (d) => fs.writeFileSync(path.join(d, '..', path.basename(d) + '-wt', 'w.txt'), 'w') },
  { git: ['status', '--porcelain'], cwd: '../{self}-wt' }, { git: ['add', '-A'], cwd: '../{self}-wt' }, { git: ['commit', '-qm', 'in worktree'], cwd: '../{self}-wt' },
  { git: ['worktree', 'list', '--porcelain'], normalizeWt: true }, ERR('worktree', 'remove', '../{self}-wt'), G('worktree', 'remove', '--force', '../{self}-wt'), G('worktree', 'list')]);

// --- remotes (local transports) ---
const REMOTE = [...HIST, SH((d) => {
  const srv = path.join(d, '..', path.basename(d) + '-srv.git');
  spawnSync(REAL, ['clone', '-q', '--bare', d, srv], { env: envFor(HOME) });
})];
C('clone local', [...HIST, { git: ['clone', '-q', '.', '../{self}-clone'] }, { git: ['status', '-sb'], cwd: '../{self}-clone' }, { git: ['branch', '-a'], cwd: '../{self}-clone' },
  { git: ['config', '--get-regexp', '^(remote|branch)\\.'], cwd: '../{self}-clone', normalizePaths: true }, { git: ['log', '--oneline', '--all'], cwd: '../{self}-clone' },
  { git: ['tag'], cwd: '../{self}-clone' }]);
C('clone bare and bundle', [...HIST, { git: ['clone', '--bare', '-q', '.', '../{self}-b.git'] }, { git: ['show-ref'], cwd: '../{self}-b.git' }, { git: ['rev-parse', 'refs/heads/main'], cwd: '../{self}-b.git' },
  G('bundle', 'create', '../{self}.bundle', 'main', 'feat'), G('bundle', 'verify', '../{self}.bundle'), G('bundle', 'list-heads', '../{self}.bundle'),
  { git: ['clone', '-q', '../{self}.bundle', '../{self}-fromb'] }, { git: ['log', '--oneline'], cwd: '../{self}-fromb' }]);
C('clone --shared borrows the source objects', [...HIST, { git: ['clone', '--bare', '-q', '--shared', '.', '../{self}-sh.git'] },
  { git: ['log', '--oneline', '--all'], cwd: '../{self}-sh.git' }, { git: ['fsck'], cwd: '../{self}-sh.git' }, { git: ['rev-parse', 'refs/heads/main'], cwd: '../{self}-sh.git' }]);
C('clone empty and errors', [...INIT, { git: ['clone', '-q', '.', '../{self}-e'], err: true }, { git: ['status'], cwd: '../{self}-e' }, { git: ['clone', '../{self}-nope', '../{self}-x'], err: true, normalizePaths: true }]);
C('fetch push local', [...REMOTE, G('remote', 'add', 'origin', '../{self}-srv.git'), G('fetch', '-q', 'origin'), G('branch', '-r'), G('fetch', 'origin', 'feat'),
  SH((d) => {}), G('rev-parse', 'FETCH_HEAD'), W('p.txt', 'p\n'), G('add', 'p.txt'), G('commit', '-qm', 'to push'), G('push', '-q', 'origin', 'HEAD:main'),
  { git: ['log', '--oneline', '-1'], cwd: '../{self}-srv.git' }, G('push', 'origin', 'HEAD:refs/heads/newb'), G('push', 'origin', ':newb'), G('push', '--force', 'origin', 'HEAD~2:feat'),
  G('push', 'origin', 'HEAD:feat'), G('push', '--force-with-lease=feat:HEAD~5', 'origin', 'HEAD:feat'), G('show-ref'), G('push', 'origin', 'HEAD:refs/converge/cycles/1'),
  { git: ['show-ref'], cwd: '../{self}-srv.git' }, G('ls-remote', 'origin'), G('ls-remote', '--heads', 'origin', 'main'), G('ls-remote', 'origin', 'nope')], { normalizeOutput: true });
C('push rejections', [...REMOTE, G('remote', 'add', 'origin', '../{self}-srv.git'), G('reset', '-q', '--hard', 'HEAD~2'), W('d', 'd'), G('add', 'd'), G('commit', '-qm', 'div'),
  { git: ['push', 'origin', 'HEAD:main'], err: true, normalizeOutput: true }, G('push', '-u', 'origin', 'HEAD:refs/heads/up'), G('status', '-sb'), G('push', 'origin', 'v1')], { normalizeOutput: true });
C('push to checked out branch refused', [...HIST, { git: ['clone', '-q', '.', '../{self}-c'] }, { git: ['commit', '--allow-empty', '-qm', 'e'], cwd: '../{self}-c' },
  { git: ['push', 'origin', 'HEAD:main'], cwd: '../{self}-c', err: true, normalizeOutput: true }], { normalizeOutput: true });
C('pull', [...REMOTE, G('remote', 'add', 'origin', '../{self}-srv.git'), G('fetch', '-q', 'origin'), G('branch', '-u', 'origin/main'), G('pull', '-q'),
  { git: ['clone', '-q', '../{self}-srv.git', '../{self}-o'] }, { git: ['commit', '--allow-empty', '-qm', 'upstream'], cwd: '../{self}-o' }, { git: ['push', '-q'], cwd: '../{self}-o' },
  G('pull', '--ff-only', '-q'), G('log', '--oneline', '-2'), G('commit', '--allow-empty', '-qm', 'local'), { git: ['commit', '--allow-empty', '-qm', 'up2'], cwd: '../{self}-o' },
  { git: ['push', '-q'], cwd: '../{self}-o' }, ERR('pull'), G('pull', '--no-rebase', '--no-edit', '-q'), G('log', '--format=%s', '-3')], { normalizeOutput: true });
C('remote ops', [...BASE, G('remote'), G('remote', 'add', 'origin', 'https://example.com/a.git'), ERR('remote', 'add', 'origin', 'x'), G('remote', '-v'), G('remote', 'get-url', 'origin'),
  G('remote', 'set-url', 'origin', 'https://example.com/b.git'), G('remote', 'get-url', 'origin'), G('remote', 'add', 'main', '../x'), G('remote'), G('remote', 'rename', 'main', 'up'),
  G('remote'), G('remote', 'remove', 'up'), G('remote'), ERR('remote', 'get-url', 'nope'), G('config', '--get-regexp', 'remote')]);
C('bundle converge shapes', [...HIST, SH((d) => {}), G('bundle', 'create', '../{self}-1.bundle', 'HEAD', '--branches'), G('bundle', 'list-heads', '../{self}-1.bundle'),
  G('update-ref', 'refs/converge-export/out', 'HEAD'), G('bundle', 'create', '../{self}-2.bundle', 'refs/converge-export/out'), G('update-ref', '-d', 'refs/converge-export/out'),
  G('bundle', 'create', '../{self}-3.bundle', 'main', '--not', 'HEAD~2'), G('bundle', 'verify', '../{self}-3.bundle'), G('bundle', 'list-heads', '../{self}-3.bundle'),
  ...INIT.map((s) => ({ ...s, cwd: '../{self}-empty' })).map((s, i) => (i === 0 ? { git: ['init', '-q', '-b', 'main', '../{self}-empty'] } : s)),
  { git: ['bundle', 'verify', '../{self}-3.bundle'], cwd: '../{self}-empty', err: true, normalizePaths: true },
  { git: ['fetch', '-q', '../{self}-1.bundle', 'HEAD:refs/import/0'], cwd: '../{self}-empty' }, { git: ['rev-parse', 'refs/import/0'], cwd: '../{self}-empty' },
  { git: ['fetch', '-q', '--no-tags', '../{self}-1.bundle', '+refs/heads/feat:refs/heads/converge'], cwd: '../{self}-empty' }, { git: ['show-ref'], cwd: '../{self}-empty' }]);
C('sources.mjs import shape', [...HIST, SH((d) => {}), G('bundle', 'create', '../{self}-imp.bundle', 'HEAD', '--branches'),
  { git: ['init', '-q', '-b', 'main', '../{self}-mono/repo'] },
  { git: ['--git-dir', '../{self}-mono/repo/.git', '--work-tree', '../{self}-mono/repo', 'read-tree', '--empty'], env: { GIT_INDEX_FILE: '../{self}-mono/index' } },
  { git: ['--git-dir', '../{self}-mono/repo/.git', '--work-tree', '../{self}-mono/repo', 'fetch', '-q', '../{self}-imp.bundle', 'HEAD:refs/import/0'] },
  { git: ['--git-dir', '../{self}-mono/repo/.git', '--work-tree', '../{self}-mono/repo', 'read-tree', '--prefix=proj/', 'refs/import/0'], env: { GIT_INDEX_FILE: '../{self}-mono/index' } },
  { git: ['--git-dir', '../{self}-mono/repo/.git', '--work-tree', '../{self}-mono/repo', 'write-tree'], env: { GIT_INDEX_FILE: '../{self}-mono/index' } },
  { git: ['--git-dir', '../{self}-mono/repo/.git', 'rev-parse', 'refs/import/0'] }]);

// --- odds and ends ---
C('unknown command and option', [...BASE, ERR('frobnicate'), { git: ['status', '--frobnicate'], errFirst: true }]);
C('-C and --git-dir', [...BASE, MKDIR('elsewhere'), { git: ['-C', '..', '-C', '{self}', 'rev-parse', 'HEAD'], cwd: 'elsewhere' }, { git: ['--git-dir=.git', 'rev-parse', 'HEAD'] },
  { git: ['--git-dir', '../{self}/.git', '--work-tree', '../{self}', 'status', '--porcelain'], cwd: 'elsewhere' }]);
C('bare repo commands from inside', [...HIST, SH((d) => spawnSync(REAL, ['clone', '-q', '--bare', d, path.join(d, '..', path.basename(d) + '-bare.git')], { env: envFor(HOME) })),
  { git: ['rev-parse', 'refs/heads/main'], cwd: '../{self}-bare.git' }, { git: ['ls-tree', '-r', '-l', 'refs/heads/main'], cwd: '../{self}-bare.git' },
  { git: ['rev-list', '--max-parents=0', 'refs/heads/main'], cwd: '../{self}-bare.git' }, { git: ['rev-parse', 'refs/heads/main:dir'], cwd: '../{self}-bare.git' },
  { git: ['cat-file', 'blob', 'refs/heads/main:a.txt'], cwd: '../{self}-bare.git' }, { git: ['ls-tree', '-r', '--name-only', 'refs/heads/main'], cwd: '../{self}-bare.git' },
  { git: ['symbolic-ref', '--short', 'HEAD'], cwd: '../{self}-bare.git' }, { git: ['config', 'receive.denyNonFastForwards', 'false'], cwd: '../{self}-bare.git' },
  { git: ['--git-dir', '../{self}-bare.git', 'show', 'refs/heads/main:a.txt'] }, { git: ['--git-dir', '../{self}-bare.git', 'diff', '--name-only', '--no-renames', '-z', '4b825dc642cb6eb9a060e54bf8d69288fbee4904', 'refs/heads/main'] }]);
C('converge agent commit cycle', [...HIST, W('work.js', 'x\n'), G('-c', 'core.hooksPath=/dev/null', '-c', 'core.fsmonitor=false', 'add', '-A'),
  G('-c', 'core.hooksPath=/dev/null', 'commit', '-q', '--no-verify', '-m', 'Work in progress'), G('status', '--porcelain'),
  G('-c', 'user.name=converge', '-c', 'user.email=a@converge.local', 'commit-tree', 'HEAD^{tree}', '-m', 'cycle 1', '-p', 'HEAD'),
  G('rev-list', '--count', 'feat..HEAD'), G('diff', '--numstat', 'HEAD~1', 'HEAD'), G('show', '--numstat', '--format=', 'HEAD'), G('status', '--porcelain=v1', '--untracked-files=all')]);
C('aliases and version', [...BASE, G('config', 'alias.st', 'status --porcelain'), W('n', 'n'), G('st')]);
C('large file count', [...INIT, SH((d) => { for (let i = 0; i < 300; i++) { fs.mkdirSync(path.join(d, `d${i % 17}`), { recursive: true }); fs.writeFileSync(path.join(d, `d${i % 17}`, `f${i}.txt`), `file ${i}\n`.repeat(i % 7 + 1)); } }),
  G('add', '-A'), G('commit', '-q', '-m', 'many'), SH((d) => { for (let i = 0; i < 300; i += 13) fs.appendFileSync(path.join(d, `d${i % 17}`, `f${i}.txt`), 'more\n'); }),
  G('status', '--porcelain'), G('diff', '--stat'), G('commit', '-qam', 'more'), G('log', '--stat', '-1', '--format=%s')]);
C('crlf and binary-ish content kept byte for byte', [...INIT, W('crlf.txt', 'a\r\nb\r\n'), W('mixed.txt', 'a\nb\r\nc'), G('add', '-A'), G('commit', '-qm', 'eol'),
  G('ls-files', '-s'), G('diff', 'HEAD', '--stat'), W('crlf.txt', 'a\r\nB\r\n'), G('diff')]);
C('symlink and gitlink entries', [...INIT, W('f', 'x'), G('add', 'f'), G('update-index', '--add', '--cacheinfo', '120000,e69de29bb2d1d6434b8b29ae775ad8c2e48c5391,link'),
  G('update-index', '--add', '--cacheinfo', '160000,1111111111111111111111111111111111111111,sub'), G('commit', '-qm', 'odd modes'), G('ls-tree', 'HEAD'), G('show', '--stat', '--format=')]);

// ---------------------------------------------------------------------------
// running
// ---------------------------------------------------------------------------
function substitute(s, dir) {
  return s.split('{self}').join(path.basename(dir));
}

function run(bin, c, dir, home) {
  const out = [];
  clock = 1700000000;
  for (const step of c.steps) {
    if (step.tick) { clock += 60; continue; }
    if (step.fs) { step.fs(dir); continue; }
    if (step.fsWt) { step.fsWt(dir); continue; }
    const cwd = step.cwd ? path.resolve(dir, substitute(step.cwd, dir)) : dir;
    const argv = step.git.map((a) => substitute(a, dir));
    let input = step.input;
    if (step.inputFile) input = fs.readFileSync(path.resolve(dir, substitute(step.inputFile, dir)));
    if (typeof input === 'string') input = Buffer.from(input);
    const env = envFor(home, Object.fromEntries(Object.entries(step.env || {}).map(([k, v]) => [k, substitute(v, dir)])));
    const useBin = step.realOnly ? REAL : bin;
    if (!fs.existsSync(cwd)) fs.mkdirSync(cwd, { recursive: true });
    const r = spawnSync(useBin, argv, { cwd, env, input, encoding: 'buffer', maxBuffer: 64 << 20, timeout: 60_000 });
    if (step.tarTo) {
      const target = path.join(dir, step.tarTo);
      fs.mkdirSync(target, { recursive: true });
      fs.writeFileSync(path.join(dir, step.tarTo + '.tar'), r.stdout);
      const t = spawnSync('tar', ['-xf', step.tarTo + '.tar', '-C', step.tarTo], { cwd: dir });
      out.push({ argv, code: r.status, stdout: `tar ${t.status} ${r.stdout.length % 512}`, stderr: '' });
      fs.rmSync(path.join(dir, step.tarTo + '.tar'));
      continue;
    }
    out.push({ argv, code: r.status ?? (r.error ? `error ${r.error.code}` : 'signal'), stdout: r.stdout ? r.stdout.toString('latin1') : '', stderr: r.stderr ? r.stderr.toString('latin1') : '', err: step.err, step });
  }
  return out;
}

function normalize(s, dir, step) {
  let t = s.split(dir.replace(/\\/g, '/')).join('<D>').split(dir).join('<D>');
  const parent = path.dirname(dir);
  t = t.split(parent.replace(/\\/g, '/')).join('<P>').split(parent).join('<P>');
  if (step && step.normalizeWt) t = t.replace(/worktree [^\n]*/g, 'worktree <path>');
  // The pack transfer messages real git prints to stderr are progress, not results.
  t = t.replace(/^(remote: |Enumerating|Counting|Compressing|Writing|Total|Unpacking|Resolving|Receiving).*\n/gm, '');
  return t;
}

function snapshot(dir) {
  // what real git sees in a repository, and the work tree's bytes
  const g = (...a) => spawnSync(REAL, a, { cwd: dir, env: envFor(HOME), encoding: 'utf8' });
  if (!fs.existsSync(path.join(dir, '.git')) && !fs.existsSync(path.join(dir, 'HEAD'))) return { none: true };
  const files = {};
  const walk = (d, rel) => {
    for (const n of fs.readdirSync(d)) {
      if (n === '.git' && !rel) continue;
      const full = path.join(d, n), r = rel ? `${rel}/${n}` : n;
      const st = fs.lstatSync(full);
      if (st.isDirectory()) walk(full, r);
      else files[r] = crypto.createHash('sha1').update(fs.readFileSync(full)).digest('hex');
    }
  };
  if (fs.existsSync(path.join(dir, '.git'))) walk(dir, '');
  return {
    head: g('rev-parse', 'HEAD').stdout.trim(),
    symref: g('symbolic-ref', '-q', 'HEAD').stdout.trim(),
    refs: g('for-each-ref', '--format=%(objectname) %(refname)').stdout,
    index: fs.existsSync(path.join(dir, '.git')) ? g('ls-files', '-s').stdout : '',
    status: fs.existsSync(path.join(dir, '.git')) ? g('status', '--porcelain=v1', '-uall').stdout : '',
    files,
  };
}

let failures = 0, stderrDiffs = 0;
const report = [];
for (const c of cases) {
  if (ONLY && !c.name.includes(ONLY)) continue;
  const base = path.join(ROOT, c.name.replace(/[^a-z0-9]+/gi, '_'));
  const dReal = path.join(base, 'real', 'wkd'), dAios = path.join(base, 'aios', 'wkd');
  const hReal = path.join(base, 'real', 'home'), hAios = path.join(base, 'aios', 'home');
  for (const d of [dReal, dAios, hReal, hAios]) fs.mkdirSync(d, { recursive: true });
  const real = run(REAL, c, dReal, hReal);
  const aios = run(AIOS, c, dAios, hAios);
  const problems = [];
  for (let i = 0; i < real.length; i++) {
    const a = aios[i], r = real[i];
    const label = `git ${r.argv.join(' ')}`;
    if (String(a.code) !== String(r.code)) problems.push(`${label}\n    exit: real ${r.code}, aios ${a.code}\n    aios stderr: ${normalize(a.stderr, dAios, a.step).trim().slice(0, 400)}`);
    const ro = normalize(r.stdout, dReal, r.step), ao = normalize(a.stdout, dAios, a.step);
    if (ro !== ao) problems.push(`${label}\n    stdout differs:\n${diffText(ro, ao)}`);
    if (r.step && r.step.errFirst) {
      const first = (t) => t.split('\n')[0];
      if (first(normalize(r.stderr, dReal, r.step)) !== first(normalize(a.stderr, dAios, a.step)))
        problems.push(`${label}\n    first stderr line differs:\n${diffText(first(r.stderr), first(a.stderr))}`);
    } else if (r.err) {
      const re = normalize(r.stderr, dReal, r.step), ae = normalize(a.stderr, dAios, a.step);
      if (re !== ae) { problems.push(`${label}\n    stderr differs:\n${diffText(re, ae)}`); }
    } else if (normalize(r.stderr, dReal, r.step) !== normalize(a.stderr, dAios, a.step)) {
      stderrDiffs++;
      if (VERBOSE) report.push(`  (stderr only) ${c.name}: ${label}\n${diffText(normalize(r.stderr, dReal, r.step), normalize(a.stderr, dAios, a.step))}`);
    }
  }
  const sr = snapshot(dReal), sa = snapshot(dAios);
  for (const k of ['head', 'symref', 'refs', 'index', 'status']) {
    if (sr[k] !== sa[k]) problems.push(`final ${k} differs:\n${diffText(String(sr[k]), String(sa[k]))}`);
  }
  if (JSON.stringify(sr.files) !== JSON.stringify(sa.files)) problems.push(`final work tree differs:\n${diffText(JSON.stringify(sr.files, null, 1), JSON.stringify(sa.files, null, 1))}`);
  if (!sa.none && fs.existsSync(path.join(dAios, '.git'))) {
    const f = spawnSync(REAL, ['fsck', '--strict', '--no-dangling'], { cwd: dAios, env: envFor(HOME), encoding: 'utf8' });
    const fr = spawnSync(REAL, ['fsck', '--strict', '--no-dangling'], { cwd: dReal, env: envFor(HOME), encoding: 'utf8' });
    if (f.status !== 0 && fr.status === 0) problems.push(`real git fsck --strict fails on aios-git's repository:\n${f.stdout}${f.stderr}`);
  }
  if (problems.length) {
    failures++;
    report.push(`FAIL ${c.name}\n  ${problems.join('\n  ')}`);
  } else if (VERBOSE) {
    report.push(`ok   ${c.name}`);
  }
}
for (const l of report) console.log(l);
const ran = cases.filter((c) => !ONLY || c.name.includes(ONLY)).length;
console.log(`\n${ran - failures}/${ran} cases agree${stderrDiffs ? ` (${stderrDiffs} steps differ only on stderr; --verbose shows them)` : ''}`);
if (!KEEP) fs.rmSync(ROOT, { recursive: true, force: true });
else console.log(`kept ${ROOT}`);
process.exit(failures ? 1 : 0);

function diffText(a, b) {
  const al = a.split('\n'), bl = b.split('\n');
  const out = [];
  const n = Math.max(al.length, bl.length);
  let shown = 0;
  for (let i = 0; i < n && shown < 14; i++) {
    if (al[i] === bl[i]) continue;
    out.push(`      real: ${JSON.stringify(al[i] ?? '<none>')}`);
    out.push(`      aios: ${JSON.stringify(bl[i] ?? '<none>')}`);
    shown++;
  }
  return out.join('\n');
}
