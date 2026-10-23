#!/usr/bin/env python3
"""serve_routes: an API-only program, and routes that run in worker VMs.

    api_test.py <grain> <api.gr>

A slow route must not hold up a fast one (workers), a route that opts out with {"workers": false} still
runs one request at a time, a route's error is a 500, and --max-body refuses a bigger body with 413.
"""
import http.client, socket, subprocess, sys, threading, time

grain, program = sys.argv[1], sys.argv[2]
failed = False

def check(name, want, got):
    global failed
    if want == got: print("ok   " + name)
    else: print(f"FAIL {name}: wanted {want!r}, got {got!r}"); failed = True

def start(*extra):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
    p = subprocess.Popen([grain, "serve", program, "--port", str(port), "--localhost", *extra],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.1)
    return p, port

def get(port, path, method="GET", body=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    c.request(method, path, body=body)
    r = c.getresponse()
    return r.status, r.read().decode()

def overlapped(port):
    """How long /fast takes while /slow (0.8 s) is running."""
    t = threading.Thread(target=get, args=(port, "/slow"))
    t.start(); time.sleep(0.2)
    began = time.time(); status, text = get(port, "/fast?2.5"); took = time.time() - began
    t.join()
    return status, text, took

server, port = start()
try:
    status, text, took = overlapped(port)
    check("fast route answers", (200, '{"doubled":5.0}'), (status, text))
    check("fast route is not held up by the slow one", True, took < 0.5)
    check("a route's error is a 500", 500, get(port, "/boom")[0])
    check("a route sees the request body", '{"n":3}', get(port, "/len", "POST", "abc")[1])
    check("an unknown path is a 404", 404, get(port, "/nope")[0])
finally:
    server.terminate(); server.wait()

server, port = start("--max-body", "10")
try:
    check("a body under the limit", 200, get(port, "/len", "POST", "0123456789")[0])
    check("a body over the limit is refused", 413, get(port, "/len", "POST", "0123456789a")[0])
finally:
    server.terminate(); server.wait()

# The same routes with {"workers": false}: one request at a time in the shared VM, as before.
program = program.replace("api.gr", "api_serial.gr")
server, port = start()
try:
    status, text, took = overlapped(port)
    check("serial: the answer is the same", (200, '{"doubled":5.0}'), (status, text))
    check("serial: the fast route waits for the slow one", True, took > 0.4)
finally:
    server.terminate(); server.wait()
sys.exit(1 if failed else 0)
