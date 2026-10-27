"""A page open when its program changes (an edited file under `grain serve`, a deploy on an app host) loads itself
again, so it is the new version's page - its markup, scripts and styles - rather than the new markup in the old one.

    reload_test.py <grain> <chrome>
"""
import os, shutil, socket, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp
from cdp import Page

grain, chrome = sys.argv[1:3]
def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); return s.getsockname()[1]
port, cdp.DEBUG_PORT = free_port(), free_port()
work = tempfile.mkdtemp()
prog = os.path.join(work, "app.gr")
def write(version):
    open(prog, "w").write('ui.pane({"title": "v", "css": ".v%d { color: rgb(1, 2, %d) }", "view": fn() { return el("p", {"id": "v", "class": "v%d"}, "version %d") }})\nrun_panes()\n' % (version, version, version, version))
write(1)
profile = tempfile.mkdtemp()
procs = [subprocess.Popen([grain, "serve", prog, "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
         subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--remote-debugging-port=%d" % cdp.DEBUG_PORT,
                           "--user-data-dir=" + profile, "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
for p_ in (port, cdp.DEBUG_PORT):
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", p_), timeout=0.2).close(); break
        except OSError: time.sleep(0.1)
ok = True
def check(name, want, got):
    global ok
    good = want == got; ok &= good
    print(("ok   " if good else "FAIL ") + name + ("" if good else f": wanted {want!r}, got {got!r}"))
try:
    p = Page("http://127.0.0.1:%d/" % port)
    time.sleep(1.0)
    check("the first version", "version 1", p.js("document.getElementById('v').textContent"))
    check("the page says its version", True, bool(p.js("document.getElementById('app').dataset.version")))
    p.js("window.__before = 'still the first load'")
    time.sleep(1.1)   # a file's version is its mtime and size: a second on
    write(2)
    for _ in range(40):   # the open stream notices within its keepalive (15 s)
        time.sleep(0.5)
        if p.js("document.getElementById('v') && document.getElementById('v').textContent") == "version 2": break
    check("the open page now shows the new version", "version 2", p.js("document.getElementById('v').textContent"))
    check("by loading again (not new markup in the old page)", None, p.js("window.__before === undefined ? null : window.__before"))
    check("with the new version's styles", "rgb(1, 2, 2)", p.js("getComputedStyle(document.getElementById('v')).color"))
finally:
    for pr in procs: pr.terminate()
    for pr in procs:
        try: pr.wait(timeout=5)
        except subprocess.TimeoutExpired: pr.kill()
    shutil.rmtree(profile, ignore_errors=True); shutil.rmtree(work, ignore_errors=True)
sys.exit(0 if ok else 1)
