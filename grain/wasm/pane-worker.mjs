// The Grain VM of one pane, in a Web Worker: the page's main thread keeps the DOM, this thread keeps the program. Its
// messages are answered in order (the module loads on the first), each with the frame the pane produced.
//
//   {id, type: "open", files: {path: text}, main, query}   writes the files, runs the program, answers its first frame
//   {id, type: "event", body}                              an action, as the page script sends it
//   {id, type: "tick"}                                     timers and the pane's refresh
//   {id, type: "close"}
import createGrain from './grain_wasm.js';
import { xhrTransport } from './http-sync.mjs';

let grain, fn;
let queue = Promise.resolve();

function json(p) {
  const answer = JSON.parse(grain.UTF8ToString(p));
  fn.free(p);
  return answer;
}

function write(path, content) {
  const parts = path.split('/').filter(Boolean);
  parts.pop();
  let dir = '';
  for (const part of parts) {
    dir += '/' + part;
    try { grain.FS.mkdir(dir); } catch { /* already there */ }
  }
  grain.FS.writeFile(path, content);
}

async function handle(m) {
  if (m.type === 'open') {
    if (!grain) {
      grain = await createGrain({ grainHttp: xhrTransport() });
      fn = {
        open: grain.cwrap('grain_pane_open', 'number', ['string', 'string']),
        event: grain.cwrap('grain_pane_event', 'number', ['string']),
        tick: grain.cwrap('grain_pane_tick', 'number', []),
        close: grain.cwrap('grain_pane_close', null, []),
        free: grain.cwrap('grain_free', null, ['number']),
      };
    }
    for (const [path, content] of Object.entries(m.files ?? {})) write(path, content);
    return json(fn.open(m.main, m.query ?? ''));
  }
  if (!grain) return { ok: false, error: 'no pane is open' };
  if (m.type === 'event') return json(fn.event(m.body));
  if (m.type === 'tick') return json(fn.tick());
  if (m.type === 'close') { fn.close(); return { ok: true }; }
  return { ok: false, error: 'unknown message ' + m.type };
}

onmessage = ({ data }) => {
  queue = queue.then(() => handle(data)).catch((e) => ({ ok: false, error: String(e && e.message ? e.message : e) }))
    .then((frame) => postMessage({ id: data.id, frame }));
};
