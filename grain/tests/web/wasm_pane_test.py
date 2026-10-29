#!/usr/bin/env python3
"""A Grain pane on the WebAssembly build, in a real Chrome: the program runs in a Web Worker, its pane is drawn into the
page's own DOM by the server's client script, its data comes from http_get, a click calls the program, and its state is
its variables. No iframe, no server running grain.

    wasm_pane_test.py <build dir with grain_wasm.js and grain-pane.js> <chrome>
"""
import os, shutil, socket, subprocess, sys, tempfile, time
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import cdp
from cdp import Page

build, chrome = sys.argv[1:3]
wasm = os.path.normpath(os.path.join(here, "..", "..", "wasm"))
site = tempfile.mkdtemp()
for name in ("grain_wasm.js", "grain_wasm.wasm", "grain-pane.js", "grain-pane.css"):
    shutil.copy(os.path.join(build, name), site)
for name in ("pane-host.mjs", "pane-worker.mjs", "http-sync.mjs"):
    shutil.copy(os.path.join(wasm, name), site)
shutil.copytree(os.path.join(wasm, "demo", "app"), os.path.join(site, "app"))
shutil.copy(os.path.join(wasm, "demo", "data.json"), site)
shutil.copy(os.path.join(wasm, "demo", "index.html"), site)

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); return s.getsockname()[1]
port, cdp.DEBUG_PORT = free_port(), free_port()
profile = tempfile.mkdtemp()
procs = [subprocess.Popen([sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1"], cwd=site, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
         subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--remote-debugging-port=%d" % cdp.DEBUG_PORT,
                           "--user-data-dir=" + profile, "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
for p in (port, cdp.DEBUG_PORT):
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", p), timeout=0.2).close(); break
        except OSError: time.sleep(0.1)
ok = True
def check(name, want, got):
    global ok
    good = want == got; ok &= good
    print(("ok   " if good else "FAIL ") + name + ("" if good else f": wanted {want!r}, got {got!r}"))

try:
    p = Page("http://127.0.0.1:%d/" % port)
    for _ in range(150):
        if p.js("window.paneReady === true"): break
        time.sleep(0.1)
    check("the pane started", [True, None], [p.js("window.paneReady === true"), p.js("window.paneError ?? null")])
    names = "Array.from(document.querySelectorAll('#app td.who')).map(function(t){return t.textContent}).join(',')"
    text = lambda sel: p.js("document.querySelector('%s').textContent" % sel)
    check("the program's view is in the page's DOM, no iframe", [True, 0], [p.js("!!document.querySelector('#app .demo h1')"), p.js("document.querySelectorAll('iframe').length")])
    check("its data came from http_get in the worker, busiest first", "Ann Lee,Bob Ray,Cy Moss,Dee Park,Eli Fox,Fay Ono", p.js(names))
    check("the pane's own css is applied", "pointer", p.js("getComputedStyle(document.querySelector('#app th.sortable')).cursor"))
    check("it runs in a worker, not on the page's thread", True, p.js("typeof window.pane === 'object' && typeof createGrain === 'undefined'"))

    check("the program starts at zero", "Clicked 0 times", text("#app .clicks"))
    for _ in range(3):
        p.js("document.querySelector('#app button.count').click()"); time.sleep(0.5)
    check("a click calls the program; its variables keep their state", "Clicked 3 times", text("#app .clicks"))

    p.js("document.querySelector('#app th.sortable').click()"); time.sleep(0.6)
    check("a click on a <th> runs its action: the order turned", "Fay Ono,Eli Fox,Dee Park,Cy Moss,Bob Ray,Ann Lee", p.js(names))
    check("the header says so", "asc", p.js("document.querySelector('#app th.sortable').dataset.dir"))
    check("and the state from before is still there", "Clicked 3 times", text("#app .clicks"))

    p.js("var i=document.querySelector('#app input[data-id=query]'); i.value='o'; i.dispatchEvent(new Event('input',{bubbles:true}))")
    p.js("document.querySelector('#app button.filter').click()"); time.sleep(0.6)
    check("a field's text goes with the action: the filter ran", "Fay Ono,Eli Fox,Cy Moss,Bob Ray", p.js(names))
    check("the field kept what was typed", "o", p.js("document.querySelector('#app input[data-id=query]').value"))
    check("a click after a filter still works", "Clicked 4 times", (p.js("document.querySelector('#app button.count').click()"), time.sleep(0.5), text("#app .clicks"))[2])
finally:
    for x in procs: x.terminate()
    shutil.rmtree(profile, ignore_errors=True)
    shutil.rmtree(site, ignore_errors=True)
sys.exit(0 if ok else 1)
