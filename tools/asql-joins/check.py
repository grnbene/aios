#!/usr/bin/env python3
"""One ASQL join case, end to end through the aios binary: build it (incrementally), load the case's setup into a
fresh root, run its statements and queries, and compare every result with what the case expects.

    python3 tools/asql-joins/check.py <case>        # tools/asql-joins/cases/<case>.case

A case file is sections, each starting with a line `-- <kind>`:

    -- setup          SQL loaded once (sql import), before anything else
    -- exec           one statement run on its own (a DELETE, an UPDATE); it must succeed
    -- query          one SELECT; the `-- expect` section after it holds its rows
    -- expect         rows as cells joined by |, any order (an empty section: no rows)
    -- maxms N        the query above must take at most N ms (a join that cross-joins first does not)

Exit 0 when every query returns exactly its rows (and in time), 1 otherwise, with each difference printed. Runs with
no network: the engine is the aios binary itself, not a server.
"""
import os, re, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BUILD = os.environ.get('ASQL_CHECK_BUILD', os.path.join(ROOT, 'build'))


def fail(msg):
    print(msg)
    sys.exit(1)


def build():
    if not os.path.exists(os.path.join(BUILD, 'build.ninja')):
        r = subprocess.run(['cmake', '-S', ROOT, '-B', BUILD, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release'], capture_output=True, text=True)
        if r.returncode != 0: fail('configure failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])
    r = subprocess.run(['cmake', '--build', BUILD, '--target', 'aios'], capture_output=True, text=True)
    if r.returncode != 0: fail('build failed:\n' + (r.stdout + r.stderr)[-4000:])
    return os.path.join(BUILD, 'aios')


def parse_case(path):
    secs, cur = [], None
    for line in open(path, encoding='utf-8'):
        m = re.match(r'^--\s*(setup|exec|query|expect|maxms)\b\s*(.*)$', line.rstrip('\n'))
        if m:
            cur = {'kind': m.group(1), 'arg': m.group(2).strip(), 'lines': []}
            secs.append(cur)
        elif cur is not None and line.strip() and not line.lstrip().startswith('--'):
            cur['lines'].append(line.rstrip('\n'))
    return secs


def run_aios(aios, root, cmd):
    t0 = time.time()
    r = subprocess.run([aios, '--root', root, 'exec', cmd], capture_output=True, text=True, env={**os.environ, 'AIOS_NO_ACCEL': '1'})
    return r.stdout + r.stderr, (time.time() - t0) * 1000


def table_rows(out):
    """The rows of the last result table printed, as tuples of cells; None if the query failed."""
    if '[SQL ERROR]' in out or '[SQL RESULT]' not in out: return None
    body = out.split('[SQL RESULT]')[-1]
    lines = [l for l in body.splitlines() if l.startswith('|')]
    rows = [tuple(c.strip() for c in l.strip().strip('|').split('|')) for l in lines[1:]]   # [0] is the header
    return rows


def norm(cell):
    try: return format(float(cell), '.4f')
    except ValueError: return cell


def main():
    if len(sys.argv) != 2: fail(__doc__)
    case = os.path.join(ROOT, 'tools', 'asql-joins', 'cases', sys.argv[1] + '.case')
    if not os.path.exists(case): fail('no such case: ' + case)
    aios = build()
    secs = parse_case(case)
    bad = 0
    with tempfile.TemporaryDirectory() as tmp:
        root = os.path.join(tmp, 'root')
        setup = '\n'.join(l for s in secs if s['kind'] == 'setup' for l in s['lines'])
        sf = os.path.join(tmp, 'setup.sql')
        open(sf, 'w').write(setup)
        out, _ = run_aios(aios, root, f'sql import file={sf}')
        if '[IMPORT SUCCESS]' not in out or 'failed' in out.lower(): fail('setup failed:\n' + out[-3000:])
        last_query, last_ms = None, 0.0
        for s in secs:
            if s['kind'] == 'exec':
                stmt = ' '.join(s['lines'])
                ef = os.path.join(tmp, 'exec.sql')
                open(ef, 'w').write(stmt.rstrip(';') + ';\n')
                out, _ = run_aios(aios, root, f'sql import file={ef}')
                if '[IMPORT SUCCESS]' not in out or re.search(r'error|failed', out.split('[IMPORT SUCCESS]')[-1], re.I):
                    print(f'FAIL  exec: {stmt}\n      {out.strip().splitlines()[-1] if out.strip() else "(no output)"}')
                    bad += 1
                else:
                    print(f'ok    exec: {stmt[:100]}')
            elif s['kind'] == 'query':
                last_query = ' '.join(s['lines'])
                qf = last_query.replace('"', '\\"')
                out, last_ms = run_aios(aios, root, f'sql query "{qf}"')
                s['rows'] = table_rows(out)
                s['out'] = out
                last = s
            elif s['kind'] == 'expect':
                want = sorted(tuple(norm(c.strip()) for c in l.split('|')) for l in s['lines'])
                got = last.get('rows')
                if got is None:
                    msg = [l for l in last['out'].splitlines() if 'ERROR' in l]
                    print(f'FAIL  {last_query}\n      {msg[-1] if msg else last["out"][-300:]}')
                    bad += 1
                    continue
                got = sorted(tuple(norm(c) for c in r) for r in got)
                if got != want:
                    print(f'FAIL  {last_query}\n      got  {got[:8]}{" ..." if len(got) > 8 else ""} ({len(got)} rows)\n      want {want[:8]}{" ..." if len(want) > 8 else ""} ({len(want)} rows)')
                    bad += 1
                else:
                    print(f'ok    {last_query[:110]}')
            elif s['kind'] == 'maxms':
                limit = float(s['arg'])
                if last_ms > limit:
                    print(f'FAIL  too slow: {last_ms:.0f} ms > {limit:.0f} ms: {last_query}')
                    bad += 1
    print(f'\n{sys.argv[1]}: {"all passed" if not bad else f"{bad} failed"}')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
