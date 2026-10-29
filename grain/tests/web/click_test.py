#!/usr/bin/env python3
"""A click on any element with an action, in a real browser: a sortable <th> (the docs' own example,
`el("th", {"on_click": fn() { ... }}, "Touches")`), not only a <button>.

    click_test.py <grain> <chrome> <markup.gr>
"""
import os, shutil, socket, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp
from cdp import Page

grain, chrome, program = sys.argv[1:4]
def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); return s.getsockname()[1]
port, cdp.DEBUG_PORT = free_port(), free_port()
profile = tempfile.mkdtemp()
procs = [subprocess.Popen([grain, "serve", program, "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
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
    names = "Array.from(document.querySelectorAll('td.who')).map(function(t){return t.textContent}).join(',')"
    check("the page is up, busiest first", "Bob,Cy,Ann <a&b>", p.js(names))
    check("the header is not a button", ["TH", False], p.js("var t=document.querySelector('th.sortable');[t.tagName, t.tagName==='BUTTON']"))
    p.js("document.querySelector('th.sortable').click()"); time.sleep(0.8)
    check("clicking the <th> ran its action: the order turned", "Ann <a&b>,Cy,Bob", p.js(names))
    check("and the header says so", "asc", p.js("document.querySelector('th.sortable').dataset.dir"))
    p.js("document.querySelector('th.sortable').click()"); time.sleep(0.8)
    check("a second click turns it back", "Bob,Cy,Ann <a&b>", p.js(names))
    # the stylesheet styles #app (width, margins): an update must leave the element as it was
    check("an update leaves the pane's own element alone (its id and its width variable)", [True, True],
          p.js("var a=document.querySelector('#app'); [a !== null, a !== null && a.style.getPropertyValue('--w') !== '']"))
finally:
    for x in procs: x.terminate()
    shutil.rmtree(profile, ignore_errors=True)
sys.exit(0 if ok else 1)
