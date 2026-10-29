// A Grain pane in a page, without an iframe: the program runs in a Web Worker (pane-worker.mjs), and what it draws is put
// into `container` by the same client the server's page runs (grain-pane.js, written from grain_webui_assets.inc).
//
//   import { mountPane } from './pane-host.mjs';
//   const pane = await mountPane(document.querySelector('#mount'), {
//     files: { '/app/main.gr': source, '/app/lib/x.gr': more },   // the program and what it imports, by path
//     main: '/app/main.gr', query: location.search.slice(1) });   // the page's query is the pane's on_open argument
//   pane.close();
//
// The page also needs grain-pane.css. The styles are not scoped yet: the pane's own CSS (its `css` option) applies to the
// whole page, as it does on the server's page.
import { grainPane } from './grain-pane.js';

export async function mountPane(container, { files, main, query = '', worker = new URL('./pane-worker.mjs', import.meta.url), tickMs = 500 }) {
  const w = new Worker(worker, { type: 'module' });
  const pending = new Map();
  let next = 0;
  w.onmessage = ({ data }) => {
    const done = pending.get(data.id);
    if (!done) return;
    pending.delete(data.id);
    for (const line of data.frame.log ?? []) console.log(line);   // what the program printed
    done(data.frame);
  };
  w.onerror = (e) => {
    for (const done of pending.values()) done({ ok: false, error: e.message || 'the pane stopped' });
    pending.clear();
  };
  const call = (message) => new Promise((done) => { const id = ++next; pending.set(id, done); w.postMessage({ ...message, id }); });

  const first = await call({ type: 'open', files, main, query });
  if (!first.ok) {
    w.terminate();
    const where = first.line ? ` (line ${first.line})` : '';
    throw new Error(`${first.error}${where}`);
  }
  const style = document.createElement('style');
  style.textContent = first.css || '';
  const app = document.createElement('div');
  app.id = 'app';
  app.style.setProperty('--w', `${first.width > 0 ? first.width : 720}px`);
  app.dataset.seq = String(first.seq);
  app.dataset.version = first.version;
  app.dataset.refresh = String(first.refresh);
  if (first.theme) { app.dataset.theme = first.theme; app.classList.add(`theme-${first.theme}`); }
  app.innerHTML = first.html;
  container.replaceChildren(style, app);

  let timer = null;
  grainPane(app, {
    send: (body) => call({ type: 'event', body }),
    subscribe: (onFrame) => {
      timer = setInterval(async () => {
        if (document.hidden) return;
        const frame = await call({ type: 'tick' });
        if (frame.ok && frame.html !== undefined) onFrame(frame);
      }, tickMs);
    },
    reload: () => {},   // a newer version of the program: the page that embeds the pane decides what to do
    hidden: () => document.hidden,
  });
  return {
    app,
    title: first.title,
    close() { clearInterval(timer); call({ type: 'close' }); w.terminate(); container.replaceChildren(); },
  };
}
