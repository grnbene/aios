#!/usr/bin/env python3
"""`grain open` on a pane drawn by hand (create_pane with on_draw): the server's pictures in a native window, and
that window's mouse and keys back to the program as its own events. Headless (GRAIN_VIDEO=dummy), scripted input;
the client prints what its picture shows where the script pressed and dragged.

    native_canvas_test.py <grain> <paint.gr>
"""
import json, os, socket, subprocess, sys, tempfile, time

grain, program = sys.argv[1], sys.argv[2]
failed = False
def check(name, ok, got=""):
    global failed
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {got!r}")); failed |= not ok

with socket.socket() as s:
    s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
server = subprocess.Popen([grain, "serve", program, "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=0.2).close(); break
        except OSError: time.sleep(0.05)
    script = [{"type": "mouse_down", "x": 50, "y": 50, "button": "left"}, {"type": "mouse_up", "x": 50, "y": 50, "button": "left"},
              {"type": "mouse_down", "x": 100, "y": 60, "button": "right"}, {"type": "mouse_up", "x": 100, "y": 60, "button": "right"},
              {"type": "mouse_move", "x": 150, "y": 150},
              {"type": "mouse_down", "x": 200, "y": 100, "button": "left"}, {"type": "mouse_move", "x": 220, "y": 120},
              {"type": "mouse_up", "x": 240, "y": 140, "button": "left"}]
    f = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False); json.dump(script, f); f.close()
    r = subprocess.run([grain, "open", f"http://127.0.0.1:{port}/", "--script", f.name, "--frames", "40", "--dump"],
                       env=dict(os.environ, GRAIN_VIDEO="dummy"), capture_output=True, text=True, timeout=60)
    out = r.stdout.splitlines()
    check("grain open shows a drawn pane", r.returncode == 0 and "picture: 300x200" in out, r.stdout + r.stderr)
    check("a left click comes back drawn by the program", "mouse_down 50,50: #40ff80" in out, out)
    check("a right click is a right click", "mouse_down 100,60: #ff4040" in out, out)
    check("a hover is not sent", "mouse_move 150,150: #101820" in out, out)
    check("a drag is", "mouse_move 220,120: #4080ff" in out, out)
    frames = [int(l.split()[1]) for l in out if l.startswith("frames: ")]
    check("each answer is a new picture", frames and frames[0] >= 4, out)
finally:
    server.kill(); server.wait()
sys.exit(1 if failed else 0)
