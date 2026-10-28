// The WebAssembly build, checked: what a program prints and returns, its errors with their lines, the step limit, the
// in-memory file system, and what a page cannot do said as an error.
//
//   node grain/wasm/test.mjs <grain_wasm.js>
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const { default: createGrain } = await import(pathToFileURL(resolve(process.argv[2] ?? 'grain_wasm.js')).href);
const grain = await createGrain();
const run_raw = grain.cwrap('grain_run', 'number', ['string', 'string', 'number']);
const free = grain.cwrap('grain_free', null, ['number']);
function run(source, limit = 0) {
  const p = run_raw(source, 'test.gr', limit);
  const answer = JSON.parse(grain.UTF8ToString(p));
  free(p);
  return answer;
}

let failed = 0;
function check(name, want, got) {
  const ok = JSON.stringify(want) === JSON.stringify(got);
  if (!ok) failed++;
  console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok ? '' : `: wanted ${JSON.stringify(want)}, got ${JSON.stringify(got)}`));
}

// what a program prints, and its last expression
let a = run('print("a")\nprint(1 + 2)\n40 + 2');
check('prints in order', ['a', '3'], a.output);
check('the last expression is the value', ['42', true], [a.value, a.ok]);

// the standard library: values checked against known answers, not against this build
check('sha256 of "abc"', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad', run('sha256("abc")').value);
check('json round trip', '{"a":[1,2,{"b":null}],"c":"x\\ny"}', run('json_stringify(json_parse("{\\"a\\":[1,2,{\\"b\\":null}],\\"c\\":\\"x\\\\ny\\"}"))').value);
check('regex_replace with groups', '28/10/2026', run('regex_replace("(\\\\d+)-(\\\\d+)-(\\\\d+)", "2026-10-28", "$3/$2/$1")').value);
check('number_format', '1,234,567.9', run('number_format(1234567.891, 1)').value);
check('date_format of a database date', 'Oct 28, 2026 2:05pm', run('date_format("2026-10-28 14:05:09", "%b %-d, %Y %-I:%M%P")').value);
check('closures and recursion', '6765', run('fn fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2) }\nfib(20)').value);
check('sort_by on maps', ['Bob', 'Ann'], run('for p in sort_by([{"n": "Ann", "t": 120}, {"n": "Bob", "t": 75}], fn(p) { return p.t }) { print(p.n) }').output);

// errors say where
a = run('let x = 1\nlet = 5');
check('a compile error names its line', [false, 'compile', 2], [a.ok, a.stage, a.line]);
a = run('print("before")\n\nno_such_function()');
check('a run error names its line, and keeps what was printed', [false, 'run', 3, ['before']], [a.ok, a.stage, a.line, a.output]);
check('the run error says what is wrong', true, a.error.includes('no_such_function'));

// the step limit stops a program that would never end
a = run('let n = 0\nwhile true { n = n + 1 }', 200000);
check('the step limit ends a runaway loop', [false, true], [a.ok, a.error.includes('step limit')]);

// files are the page's in-memory file system
a = run('write_file("/note.txt", "hello")\nprint(read_file("/note.txt"))\nprint(str(file_exists("/note.txt")))');
check('write then read a file', [true, ['hello', 'true']], [a.ok, a.output]);
check('reading a missing file is an error', false, run('read_file("/nope.txt")').ok);

// what a page cannot do is an error that says so
a = run('process_run(["ls"])');
check('process_run says it is not available in a browser', [false, true], [a.ok, a.error.includes('not available in a browser')]);

// each run is a VM of its own: nothing leaks between programs
run('let leaked = 1');
check('globals do not leak between runs', false, run('leaked').ok);

console.log(failed === 0 ? 'PASS: grain wasm' : `FAIL: ${failed} check(s)`);
process.exit(failed === 0 ? 0 : 1);
