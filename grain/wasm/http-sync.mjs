// The synchronous request function the VM's http_* builtins call (Module.grainHttp, see http_browser.cpp).
//
// A Grain program calls http_get and waits for the answer; WebAssembly cannot wait for a promise, so the page gives
// the VM a function that blocks. In a Web Worker that is a synchronous XMLHttpRequest (allowed there, and it blocks
// only the worker); in Node, where there is none, a helper thread does the fetch while this one waits on Atomics.
//
//   const grain = await createGrain({ grainHttp: httpTransport() });
//
// The answer is text: "ok", status, reason, header count, name and value lines, then the body as base64 - or "error"
// and a message. A browser undoes gzip and follows redirects itself, so content-encoding and content-length are
// dropped from the headers (the body no longer matches them).

const FORBIDDEN = new Set(['accept-charset', 'accept-encoding', 'access-control-request-headers', 'access-control-request-method',
  'connection', 'content-length', 'cookie', 'cookie2', 'date', 'dnt', 'expect', 'host', 'keep-alive', 'origin', 'referer',
  'set-cookie', 'te', 'trailer', 'transfer-encoding', 'upgrade', 'via']);
const DROPPED = new Set(['content-encoding', 'content-length']);

function answer(status, reason, headers, bytes) {
  const lines = ['ok', String(status), reason, String(headers.length)];
  for (const [k, v] of headers) lines.push(k, v);
  lines.push(toBase64(bytes));
  return lines.join('\n');
}

function toBase64(bytes) {
  if (typeof Buffer !== 'undefined') return Buffer.from(bytes).toString('base64');
  let s = '';
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

function fromBase64(text) {
  if (typeof Buffer !== 'undefined') return new Uint8Array(Buffer.from(text, 'base64'));
  const s = atob(text);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

// In a Web Worker (or a page that accepts the deprecation warning): a synchronous XMLHttpRequest.
export function xhrTransport() {
  return (requestJson) => {
    const r = JSON.parse(requestJson);
    const xhr = new XMLHttpRequest();
    try {
      xhr.open(r.method, r.url, false);
      xhr.responseType = 'arraybuffer';
      try { xhr.timeout = r.timeout_ms; } catch { /* a page's synchronous request takes none */ }
      for (const [k, v] of r.headers) {
        const low = k.toLowerCase();
        if (FORBIDDEN.has(low) || low.startsWith('proxy-') || low.startsWith('sec-')) continue;
        try { xhr.setRequestHeader(k, v); } catch { /* a header the browser refuses is not sent */ }
      }
      xhr.send(r.body ? fromBase64(r.body) : null);
    } catch (e) {
      return 'error\n' + (e && e.message ? e.message : String(e)) + ' (a network error, or a server that does not allow this page: CORS)';
    }
    if (xhr.status === 0) return 'error\nno answer (a network error, or a server that does not allow this page: CORS)';
    const headers = [];
    for (const line of xhr.getAllResponseHeaders().split('\r\n')) {
      const at = line.indexOf(':');
      if (at <= 0) continue;
      const name = line.slice(0, at).trim().toLowerCase();
      if (!DROPPED.has(name)) headers.push([name, line.slice(at + 1).trim()]);
    }
    return answer(xhr.status, xhr.statusText, headers, new Uint8Array(xhr.response));
  };
}

// In Node: a helper thread does the fetch; this thread waits for it. Answers up to 16 MB (base64 included).
export async function nodeTransport() {
  const { Worker } = await import('node:worker_threads');
  const SIZE = 16 << 20;
  const sab = new SharedArrayBuffer(8 + SIZE);
  const flag = new Int32Array(sab, 0, 2);   // [0]: 0 waiting, 1 answered; [1]: the answer's length in bytes
  const data = new Uint8Array(sab, 8);
  const worker = new Worker(`
    const { parentPort } = require('node:worker_threads');
    const DROPPED = new Set(['content-encoding', 'content-length']);
    parentPort.on('message', async ({ sab, request }) => {
      const flag = new Int32Array(sab, 0, 2), data = new Uint8Array(sab, 8);
      let text;
      try {
        const r = JSON.parse(request);
        const init = { method: r.method, headers: Object.fromEntries(r.headers), signal: AbortSignal.timeout(r.timeout_ms) };
        if (r.body) init.body = Buffer.from(r.body, 'base64');
        const res = await fetch(r.url, init);
        const bytes = Buffer.from(await res.arrayBuffer());
        const headers = [];
        res.headers.forEach((v, k) => { if (!DROPPED.has(k)) headers.push([k, v]); });
        text = ['ok', String(res.status), res.statusText, String(headers.length), ...headers.flat(), bytes.toString('base64')].join('\\n');
      } catch (e) {
        const c = e.cause && (e.cause.code || (e.cause.errors && e.cause.errors[0] && e.cause.errors[0].code));
        text = 'error\\n' + (c ? c + ': ' : '') + e.message;
      }
      let out = Buffer.from(text);
      if (out.length > data.length) out = Buffer.from('error\\nthe answer is larger than the 16 MB this transport carries');
      data.set(out);
      Atomics.store(flag, 1, out.length);
      Atomics.store(flag, 0, 1);
      Atomics.notify(flag, 0);
    });
  `, { eval: true });
  worker.unref();
  return (requestJson) => {
    Atomics.store(flag, 0, 0);
    worker.postMessage({ sab, request: requestJson });
    Atomics.wait(flag, 0, 0);
    return Buffer.from(data.subarray(0, Atomics.load(flag, 1))).toString();
  };
}
