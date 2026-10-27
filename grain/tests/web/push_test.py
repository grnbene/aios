"""A pushed update in a real browser: the text it changes is marked (ui-changed) for a moment, an element the page's
own script keeps (data-ui-keep) is left as the script made it, and the answer to the viewer's own click is not marked.

    push_test.py <grain> <chrome> <push.gr>
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
work = tempfile.mkdtemp()
data = os.path.join(work, "value.txt")
open(data, "w").write("1\n")
profile = tempfile.mkdtemp()
procs = [subprocess.Popen([grain, "serve", program, "--port", str(port)], env=dict(os.environ, WATCH_FILE=data), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
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
    time.sleep(1.0)   # the event stream open
    check("the first view", "value 1", p.js("document.getElementById('v').textContent"))
    p.js("document.getElementById('keep').textContent = 'kept by the page'")
    open(data, "w").write("2\n")
    time.sleep(0.8)
    check("the change was pushed", "value 2", p.js("document.getElementById('v').textContent"))
    check("and marked", True, p.js("document.getElementById('v').classList.contains('ui-changed')"))
    check("what did not change is not marked", False, p.js("document.getElementById('n').classList.contains('ui-changed')"))
    check("the page's own element is left as its script made it", "kept by the page", p.js("document.getElementById('keep').textContent"))
    time.sleep(2.6)
    check("the mark goes after a moment", False, p.js("document.getElementById('v').classList.contains('ui-changed')"))
    # a keyed reorder: the same elements moved, their text not marked
    p.js("document.querySelector('li[data-key=a]')._mine = 'yes'")
    open(data, "w").write("3\n")
    time.sleep(0.8)
    check("keyed rows reordered", ["row b", "row a"], p.js("[...document.querySelectorAll('#rows li')].map(l => l.textContent)"))
    check("by moving the same element", "yes", p.js("document.querySelector('li[data-key=a]')._mine || ''"))
    check("a moved row is not marked", 0, p.js("document.querySelectorAll('#rows .ui-changed').length"))
    time.sleep(2.8)
    p.js("document.getElementById('add').click()")
    time.sleep(0.6)
    check("the viewer's own click is answered", "n=1", p.js("document.getElementById('n').textContent"))
    check("and not marked", False, p.js("document.getElementById('n').classList.contains('ui-changed')"))
finally:
    for pr in procs: pr.terminate()
    for pr in procs:
        try: pr.wait(timeout=5)
        except subprocess.TimeoutExpired: pr.kill()
    shutil.rmtree(profile, ignore_errors=True); shutil.rmtree(work, ignore_errors=True)
sys.exit(0 if ok else 1)
