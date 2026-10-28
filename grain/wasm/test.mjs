// The WebAssembly build, checked: what a program prints and returns, its errors with their lines, the step limit, the
// in-memory file system, and what a page cannot do said as an error.
//
//   node grain/wasm/test.mjs <grain_wasm.js>
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
import { spawn } from 'node:child_process';
import { gzipSync } from 'node:zlib';
import { nodeTransport } from './http-sync.mjs';

// A server in a process of its own: this thread blocks while the VM waits for an answer, so it cannot serve one.
const server = spawn(process.execPath, ['-e', `
  const http = require('node:http'), zlib = require('node:zlib');
  http.createServer((req, res) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      const body = Buffer.concat(chunks).toString();
      if (req.url === '/gzip') { res.writeHead(200, { 'content-type': 'text/plain', 'content-encoding': 'gzip' }); return res.end(zlib.gzipSync('compressed hello')); }
      if (req.url === '/redirect') { res.writeHead(302, { location: '/echo?from=redirect' }); return res.end(); }
      if (req.url === '/missing') { res.writeHead(404, { 'content-type': 'text/plain' }); return res.end('no such page'); }
      if (req.url === '/binary') { res.writeHead(200, { 'content-type': 'application/octet-stream' }); return res.end(Buffer.from([0, 255, 10, 13, 128])); }
      res.writeHead(200, { 'content-type': 'application/json', 'x-seen': 'yes' });
      res.end(JSON.stringify({ method: req.method, url: req.url, body, custom: req.headers['x-custom'] || null, auth: req.headers.authorization || null }));
    });
  }).listen(0, '127.0.0.1', function () { console.log(this.address().port); });
`], { stdio: ['ignore', 'pipe', 'inherit'] });
const port = await new Promise((ok) => server.stdout.once('data', (d) => ok(Number(String(d).trim()))));
process.on('exit', () => server.kill());
const base = `http://127.0.0.1:${port}`;

const { default: createGrain } = await import(pathToFileURL(resolve(process.argv[2] ?? 'grain_wasm.js')).href);
const grain = await createGrain({ grainHttp: await nodeTransport() });
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

// http_*: the same builtins, over the page's transport
const get = (path) => run(`let r = http_get("${base}${path}")\nprint(str(r.status)); print(str(r.ok)); print(r.body); print(str(r.headers["content-type"]))`);
a = get('/echo?x=1');
check('http_get: status, ok, body and a lower-cased header', ['200', 'true', '{"method":"GET","url":"/echo?x=1","body":"","custom":null,"auth":null}', 'application/json'], a.output);
a = run(`let r = http_post("${base}/echo", "name=ann", {"headers": {"X-Custom": "yes"}})\nprint(json_parse(r.body).method + " " + json_parse(r.body).body + " " + json_parse(r.body).custom)`);
check('http_post sends its body and headers', ['POST name=ann yes'], a.output);
a = run(`let r = http_request({"url": "${base}/echo", "method": "PUT", "body": "x", "headers": {"X-Custom": "1"}})\nprint(json_parse(r.body).method)`);
check('http_request takes its method', ['PUT'], a.output);
a = get('/missing');
check('an error status is a response, not an error', ['404', 'false', 'no such page'], a.output.slice(0, 3));
a = get('/gzip');
check('a gzip body arrives plain, once', ['200', 'true', 'compressed hello'], a.output.slice(0, 3));
a = get('/redirect');
check('a redirect is followed', ['200', 'true'], a.output.slice(0, 2));
check('and its answer is the final one', true, a.output[2].includes('from=redirect'));
a = run(`let r = http_get("${base}/binary", {"headers": {}})\nprint(str(len(r.body)))`);
check('a binary body keeps its length', ['5'], a.output);
a = run(`let r = http_get("${base}/echo", {"headers": {"Authorization": "Bearer k_u"}})\nprint(json_parse(r.body).auth)`);
check('an Authorization header goes through', ['Bearer k_u'], a.output);
a = run('let r = pcall(http_get, "http://127.0.0.1:1/")\nprint(str(r.ok))\nprint(r.error)');
check('a refused connection is an error a program can catch', 'false', a.output[0]);
check('and says what failed', true, /ECONNREFUSED|fetch failed/.test(a.output[1] ?? ''));

// a VM the page gave no network says so
{
  const bare = await createGrain();
  const p = bare.cwrap('grain_run', 'number', ['string', 'string', 'number'])('http_get("http://example.com/")', 'bare.gr', 0);
  const answer = JSON.parse(bare.UTF8ToString(p));
  check('http_get with no transport given says so', [false, true], [answer.ok, answer.error.includes('gave the VM no network')]);
}

// what a page cannot do says so
for (const [call, name] of [['db_connect({})', 'db_connect'], ['tcp_connect("127.0.0.1", 80)', 'tcp_connect'], ['udp_socket()', 'udp_socket'],
                            ['mail_send({})', 'mail_send'], ['live("select 1", fn(r) { return r })', 'live']]) {
  a = run(call);
  check(`${name} says it is not available in a browser`, [false, true], [a.ok, a.error.includes(`${name}() is not available in a browser`)]);
}

console.log(failed === 0 ? 'PASS: grain wasm' : `FAIL: ${failed} check(s)`);
process.exit(failed === 0 ? 0 : 1);
