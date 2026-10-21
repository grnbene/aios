// A smart-HTTP git server for packs.sh: <git> http-backend behind node's http, pushes allowed.
//   node packs-http.mjs <dir of bare repositories> <port> <git binary>
import http from 'node:http'; import { spawn } from 'node:child_process';
const [root, port, gitBin] = process.argv.slice(2);
http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x'); const t0 = Date.now(); let bytes = 0;
  const env = { ...process.env, GIT_PROJECT_ROOT: root, GIT_HTTP_EXPORT_ALL: '1', PATH_INFO: u.pathname, REQUEST_METHOD: req.method, QUERY_STRING: u.search.slice(1), CONTENT_TYPE: req.headers['content-type'] || '', CONTENT_LENGTH: req.headers['content-length'] || '', HTTP_CONTENT_ENCODING: req.headers['content-encoding'] || '', GIT_CONFIG_COUNT: '1', GIT_CONFIG_KEY_0: 'http.receivepack', GIT_CONFIG_VALUE_0: 'true', REMOTE_USER: 't' };
  const cgi = spawn(gitBin, ['http-backend'], { env, stdio: ['pipe', 'pipe', 'inherit'] });
  req.pipe(cgi.stdin);
  let head = Buffer.alloc(0), done = false, first = 0;
  cgi.stdout.on('data', (d) => {
    bytes += d.length; if (!first) first = Date.now() - t0;
    if (!done) { head = Buffer.concat([head, d]); let i = head.indexOf('\r\n\r\n'), sep = 4; if (i < 0) { i = head.indexOf('\n\n'); sep = 2; } if (i < 0) return; done = true;
      let st = 200; const h = {};
      for (const l of head.slice(0, i).toString().split(/\r?\n/)) { const k = l.slice(0, l.indexOf(':')), v = l.slice(l.indexOf(':') + 1).trim(); if (k.toLowerCase() === 'status') st = parseInt(v); else if (k) h[k] = v; }
      res.writeHead(st, h); res.write(head.slice(i + sep)); return; }
    res.write(d);
  });
  cgi.on('close', () => { res.end(); console.error(`${req.method} ${u.pathname.split('/').pop()}: ${(bytes / 1048576).toFixed(1)} MB, first byte ${first} ms, total ${Date.now() - t0} ms`); });
}).listen(+port, '127.0.0.1');
