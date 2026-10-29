// A ui.pane on the WebAssembly build, without a page: open, an action, state kept between actions, imports, errors, the
// refresh. The apps are the ones the server's own tests use (grain/tests/web/*.gr), and the assertions are the ones
// markup_test.py and web_ui_test.py make over HTTP.
//
//   node grain/wasm/pane-test.mjs <grain_wasm.js> <grain/tests/web>
import { readFileSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';

const [, , module_path = 'grain_wasm.js', apps = 'grain/tests/web'] = process.argv;
const { default: createGrain } = await import(pathToFileURL(resolve(module_path)).href);
const grain = await createGrain();
const open_ = grain.cwrap('grain_pane_open', 'number', ['string', 'string']);
const event_ = grain.cwrap('grain_pane_event', 'number', ['string']);
const tick_ = grain.cwrap('grain_pane_tick', 'number', []);
const close_ = grain.cwrap('grain_pane_close', null, []);
const free = grain.cwrap('grain_free', null, ['number']);
const json = (p) => { const v = JSON.parse(grain.UTF8ToString(p)); free(p); return v; };
const open = (main, query = '') => json(open_(main, query));
const event = (body) => json(event_(body));
const tick = () => json(tick_());
grain.FS.mkdir('/app');
const put = (path, text) => grain.FS.writeFile(path, text);
put('/app/markup.gr', readFileSync(`${apps}/markup.gr`, 'utf8'));
put('/app/counter.gr', readFileSync(`${apps}/counter.gr`, 'utf8'));

let failed = 0;
function check(name, want, got) {
  const ok = JSON.stringify(want) === JSON.stringify(got);
  if (!ok) failed++;
  console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok ? '' : `: wanted ${JSON.stringify(want)}, got ${JSON.stringify(got)}`));
}
const names = (html) => [...html.matchAll(/<td class="who">([^<]*)<\/td>/g)].map((m) => m[1]);

// markup: el() nodes with actions, a bound field, a widget inside, escaping
let f = open('/app/markup.gr');
check('the pane opens', [true, 'Markup'], [f.ok, f.title]);
check("the pane's css comes with it", true, f.css.includes('.sortable { cursor: pointer }'));
check('the markup is the fragment, not a page', true, f.html.includes('<table><thead><tr><th>Name</th>') && !f.html.includes('<html'));
check('text is escaped', true, f.html.includes('Ann &lt;a&amp;b&gt;') && !f.html.includes('<a&b>'));
check('busiest first', ['Bob', 'Cy', 'Ann &lt;a&amp;b&gt;'], names(f.html));
check('a widget inside markup is a widget', true, /<div class="w-text bold"[^>]*>clicks 0<\/div>/.test(f.html));
const act = f.html.match(/<th class="sortable" data-dir="desc" data-act="([^"]+)">Touches<\/th>/);
check('on_click is an action on the element', true, act !== null);
check('no function leaks into the markup', false, f.html.includes('on_click'));
const seq0 = f.seq;
f = event(`action=${act[1]}`);
check('the click ran: the sort turned', ['Ann &lt;a&amp;b&gt;', 'Cy', 'Bob'], names(f.html));
check('the header says so, and the widget saw it', true, f.html.includes('data-dir="asc"') && f.html.includes('>clicks 1</div>'));
check('the frame moved on', true, f.seq > seq0);
const seq1 = f.seq;
f = event('action=');
check('a render with nothing new keeps the sort (the VM keeps it)', ['Ann &lt;a&amp;b&gt;', 'Cy', 'Bob'], names(f.html));
check('and the frame does not move', seq1, f.seq);
check('bind makes the input a field', true, f.html.includes('<input type="date" data-id="start" value="2026-10-01">'));
const run = f.html.match(/data-act="([^"]+)">Run<\/button>/)[1];
f = event(`action=${run}&f.start=2026-09-15`);
check("the action got the field's value", true, f.html.includes('<p id="start">from 2026-09-15</p>'));
check('the input shows the new value', true, f.html.includes('data-id="start" value="2026-09-15"'));
f = event(`action=${run}&f.start=2026%2D09%2D20+x`);
check('urlencoded fields are decoded', true, f.html.includes('from 2026-09-20 x'));
f = open('/app/markup.gr', 'start=2026-08-01');
check("on_open gets the page's query", true, f.html.includes('from 2026-08-01'));

// ui.* widgets: a counter whose state is the program's variables, kept between actions
f = open('/app/counter.gr');
check('a widget pane opens', [true, 'Counter'], [f.ok, f.title]);
check('it starts at zero', true, f.html.includes('Clicked 0 times'));
const add = f.html.match(/data-act="([^"]+)"[^>]*>Add one</)[1];
event(`action=${add}`); event(`action=${add}`);
f = event(`action=${add}`);
check('three clicks, three counts', true, f.html.includes('Clicked 3 times'));
const save = f.html.match(/data-act="([^"]+)"[^>]*>Save</)[1];
f = event(`action=${save}&f.note=hello+world`);
check("Enter or Save runs the action with the field's text", true, f.html.includes('hello world'));

// files and imports, from the page's file system
grain.FS.mkdir('/app/lib');
put('/app/lib/shared.gr', 'fn greet(name) { return "hello " + name }\n');
put('/app/uses_import.gr', 'import "lib/shared.gr"\nui.pane({"title": "Import", "view": fn() { return el("p", greet("page")) }})\n');
f = open('/app/uses_import.gr');
check('import resolves against the program directory', [true, true], [f.ok, f.html.includes('hello page')]);

// refresh: the view is called again every `refresh` seconds, and only a changed view is a new frame
put('/app/clock.gr', 'let n = 0\nui.pane({"title": "Clock", "refresh": 1, "view": fn() { n += 1\n return el("p", "view call " + str(n)) }})\n');
f = open('/app/clock.gr');
check('the first frame is the first view call', true, f.html.includes('view call 1'));
check('a tick before the refresh is due changes nothing', [true, false], (({ ok, changed }) => [ok, changed])(tick()));
await new Promise((r) => setTimeout(r, 1200));
const t = tick();
check('a tick after it re-renders: a new frame', [true, true], [t.ok, (t.html ?? '').includes('view call')]);

// what goes wrong says so
check('a missing program', false, open('/app/nope.gr').ok);
put('/app/nopane.gr', 'print("no pane here")\n');
f = open('/app/nopane.gr');
check('a program with no pane says so', [false, true], [f.ok, f.error.includes('no ui.pane')]);
put('/app/broken.gr', 'let = 1\n');
f = open('/app/broken.gr');
check('a compile error names its line', [false, 1], [f.ok, f.line]);
put('/app/dies.gr', 'ui.pane({"title": "x", "view": fn() { return no_such_function() }})\n');
f = open('/app/dies.gr');
check("an error in the first view is the open's error", [false, true], [f.ok, f.error.includes('no_such_function')]);
check('an event with no pane open says so', false, (close_(), event('action=x')).ok);

console.log(failed === 0 ? 'PASS: grain wasm pane' : `FAIL: ${failed} check(s)`);
process.exit(failed === 0 ? 0 : 1);
