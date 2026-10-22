#!/usr/bin/env python3
"""`grain open`: a ui.pane program running on a server, in a native window - no HTML. The window runs headless
(GRAIN_VIDEO=dummy) with scripted input, and the test reads back what it showed.

    native_client_test.py <grain> <counter.gr>
    GRAIN_WEB_PORT=n native_client_test.py <grain>     against a server already listening there
"""
import json, os, socket, subprocess, sys, tempfile, time

grain = sys.argv[1]
failed = False
def check(name, want, got):
    global failed
    if want == got: print("ok   " + name)
    else: print(f"FAIL {name}: wanted {want!r}, got {got!r}"); failed = True

if os.environ.get("GRAIN_WEB_PORT"):
    port, server = int(os.environ["GRAIN_WEB_PORT"]), None
else:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
    server = subprocess.Popen([grain, "serve", sys.argv[2], "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
env = dict(os.environ, GRAIN_VIDEO="dummy")
def open_app(script=None, frames=20):
    args = [grain, "open", f"http://127.0.0.1:{port}/", "--frames", str(frames), "--dump"]
    if script:
        f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False); json.dump(script, f); f.close()
        args += ["--script", f.name]
    r = subprocess.run(args, env=env, capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout.splitlines(), r.stderr

try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)

    rc, lines, err = open_app()
    check("it connects and builds the widgets", 0, rc)
    check("the program's text arrives", True, "Clicked 0 times" in lines and "Add one" in lines and "Reset" in lines)

    rc, lines, err = open_app([{"type": "text", "text": "hello native"}, {"type": "key", "key": "return"},
                               {"type": "text", "text": "second"}, {"type": "key", "key": "return"}])
    check("Enter runs the server's on_enter with the field text", True, '{"text":"hello native","n":0}' in lines and '{"text":"second","n":0}' in lines)
    check("what the program cleared in the field shows in the window", True, "[]" in lines)

    rc, lines, err = open_app([{"type": "text", "text": "x"}, {"type": "key", "key": "return"}])
    check("each window is its own session", True, '{"text":"x","n":0}' in lines and not any("hello native" in l for l in lines))

    refused = subprocess.run([grain, "open", "http://127.0.0.1:1/", "--frames", "1"], env=env, capture_output=True, text=True, timeout=30)
    check("an unreachable server is an error, not a hang", True, refused.returncode != 0 and "cannot reach" in refused.stderr)
    if server:
        plain = subprocess.run([grain, "open", "http://127.0.0.1:%d/" % port, "--bogus"], env=env, capture_output=True, text=True, timeout=30)
        check("an unknown option is refused", True, plain.returncode != 0 and "unknown option" in plain.stderr)
finally:
    if server: server.kill(); server.wait()
sys.exit(1 if failed else 0)
