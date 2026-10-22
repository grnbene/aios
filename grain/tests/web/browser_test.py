"""The page script, in a real browser: clicks, Enter, focus and caret kept across redraws, escaping, refresh.

    browser_test.py <grain> <chrome> <counter.gr> <sysmon.gr> <live.gr>
"""
import os, shutil, socket, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp
from cdp import Page

grain, chrome, counter, sysmon, live_app = sys.argv[1:6]
def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); return s.getsockname()[1]
p1, p2, p3, cdp.DEBUG_PORT = free_port(), free_port(), free_port(), free_port()
profile = tempfile.mkdtemp()
procs = [subprocess.Popen([grain, "serve", counter, "--port", str(p1)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
         subprocess.Popen([grain, "serve", sysmon, "--port", str(p2)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
         subprocess.Popen([grain, "serve", live_app, "--port", str(p3)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
         subprocess.Popen([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--remote-debugging-port=%d" % cdp.DEBUG_PORT,
                           "--user-data-dir=" + profile, "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
for port in (p1, p2, p3, cdp.DEBUG_PORT):
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.1)
ok = True
def check(name, want, got):
    global ok
    good = want == got; ok &= good
    print(("ok   " if good else "FAIL ") + name + ("" if good else f": wanted {want!r}, got {got!r}"))
p = Page("http://127.0.0.1:%d/" % p1)
check("initial text", "Clicked 0 times", p.js("document.querySelector('.w-text').textContent"))
check("autofocus on the note field", "note", p.js("document.activeElement.dataset.id"))
p.js("document.querySelector('.w-btn.primary').click()"); time.sleep(0.5)
check("a click calls the grain function", "Clicked 1 times", p.js("document.querySelector('.w-text').textContent"))
p.js("document.querySelector('.w-btn.primary').click(); document.querySelector('.w-btn.primary').click()"); time.sleep(0.8)
check("clicks sent together are not lost", "Clicked 3 times", p.js("document.querySelector('.w-text').textContent"))
# typing then Enter: the field text goes to the server, the app saves it and clears the field
p.js("var i=document.querySelector('input[data-id=note]'); i.focus(); i.value='from the browser'; i.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',bubbles:true}))"); time.sleep(0.6)
check("Enter runs on_enter with the field text", "from the browser", p.js("document.querySelector('.w-table td').textContent"))
check("the app cleared the field", "", p.js("document.querySelector('input[data-id=note]').value"))
check("focus stays in the field after a redraw", "note", p.js("document.activeElement.dataset.id"))
# a caret in the middle of typed text survives a redraw
p.js("var i=document.querySelector('input[data-id=note]'); i.focus(); i.value='abcdef'; i.setSelectionRange(2,4)")
p.js("document.querySelector('.w-btn:not(.primary)').click()"); time.sleep(0.6)   # Reset: redraws without touching the field
check("typed text survives a redraw", "abcdef", p.js("document.querySelector('input[data-id=note]').value"))
check("the caret survives a redraw", [2, 4], p.js("var i=document.querySelector('input[data-id=note]');[i.selectionStart,i.selectionEnd]"))
check("Reset took effect", "Clicked 0 times", p.js("document.querySelector('.w-text').textContent"))
check("text is escaped, not executed", None, p.js("(function(){var i=document.querySelector('input[data-id=note]');i.value='<img src=x onerror=window.pwned=1>';return null})()"))
p.js("document.querySelector('.w-btn:not(.primary):last-of-type') && 0")
p.js("document.querySelectorAll('.w-btn')[2].click()"); time.sleep(0.6)   # Save the note
check("no script injection from field text", None, p.js("window.pwned"))
check("the markup came back as text", True, "<img src=x onerror=window.pwned=1>" in p.js("Array.from(document.querySelectorAll('.w-table tbody tr td:first-child')).map(function(t){return t.textContent})"))
# live data: a shared app whose clock ticks and whose messages arrive with no request from this page
live = Page("http://127.0.0.1:%d/" % p3, cdp.new_tab())
first = live.js("document.querySelectorAll('.w-tile .tv')[0].textContent")
time.sleep(2.6)
check("the page updates by itself as the data changes (EventSource)", True, int(live.js("document.querySelectorAll('.w-tile .tv')[0].textContent")) >= int(first) + 2)
other = Page("http://127.0.0.1:%d/" % p3, cdp.new_tab())
other.js("var i=document.querySelector('input[data-id=words]'); i.value='hi from another tab'; document.querySelectorAll('.w-btn')[0].click()"); time.sleep(0.8)
check("another viewer's message appears with nothing done here", True, "hi from another tab" in live.js("document.body.innerText"))
# sysmon refreshes on its own
q = Page("http://127.0.0.1:%d/" % p2, cdp.new_tab())
a = p2 = q.js("document.querySelectorAll('.w-tile .tv')[3] ? document.querySelectorAll('.w-tile .tv')[3].textContent : document.querySelectorAll('.w-tile .tv')[2].textContent")
time.sleep(2.6)
b = q.js("document.querySelectorAll('.w-tile .tv')[3] ? document.querySelectorAll('.w-tile .tv')[3].textContent : document.querySelectorAll('.w-tile .tv')[2].textContent")
check("a pane with refresh redraws by itself", True, a != b)
for pr in procs: pr.kill()
shutil.rmtree(profile, ignore_errors=True)
sys.exit(0 if ok else 1)
