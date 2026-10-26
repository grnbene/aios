#!/usr/bin/env python3
import http.client, json, socket, subprocess, sys, time

grain, program = sys.argv[1], sys.argv[2]
failed = False

def check(name, want, got):
    global failed
    if want == got:
        print("ok   " + name)
    else:
        print(f"FAIL {name}: wanted {want!r}, got {got!r}")
        failed = True

def start(*extra):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    p = subprocess.Popen([grain, "serve", program, "--port", str(port), "--localhost", *extra],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    return p, port

def req(port, path, method="GET", body=None, headers=None):
    if headers is None: headers = {}
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    c.request(method, path, body=body, headers=headers)
    r = c.getresponse()
    return r.status, r.read().decode(), r

server, port = start()
try:
    # 1. GET /items with query parameters
    status, body, resp = req(port, "/items?filter=active&page=2")
    check("GET /items status", 200, status)
    data = json.loads(body)
    check("GET /items filter query param", "active", data.get("filter"))
    check("GET /items page query param", "2", data.get("page"))

    # 2. POST /items with JSON body and custom response header
    item_payload = json.dumps({"name": "Special Widget", "qty": 42})
    status, body, resp = req(port, "/items", method="POST", body=item_payload, headers={"Content-Type": "application/json"})
    check("POST /items status 201", 201, status)
    check("POST /items reason phrase", "Created", resp.reason)
    check("POST /items custom header", "1234", resp.getheader("X-Resource-Id"))
    post_data = json.loads(body)
    check("POST /items item name", "Special Widget", post_data.get("item", {}).get("name"))
    check("POST /items item qty", 42, post_data.get("item", {}).get("qty"))

    # 3. GET /items/:id parameter route
    status, body, resp = req(port, "/items/sku-999")
    check("GET /items/:id status", 200, status)
    data = json.loads(body)
    check("GET /items/:id param value", "sku-999", data.get("id"))
    check("GET /items/:id found", True, data.get("found"))

    # 4. GET /files/*path wildcard route
    status, body, resp = req(port, "/files/docs/api/v1.txt")
    check("GET /files/*path status", 200, status)
    check("GET /files/*path body", "File content at docs/api/v1.txt", body)

    # 5. GET /redirect res_redirect helper
    status, body, resp = req(port, "/redirect")
    check("GET /redirect status 302", 302, status)
    check("GET /redirect reason phrase", "Found", resp.reason)
    check("GET /redirect Location header", "/target", resp.getheader("Location"))

    # 6. POST /error res_error helper
    status, body, resp = req(port, "/error", method="POST")
    check("POST /error status 422", 422, status)
    check("POST /error reason phrase", "Unprocessable Entity", resp.reason)
    err_data = json.loads(body)
    check("POST /error json structure", "Validation failed", err_data.get("error"))

    # 7. Method mismatch (e.g. DELETE /items/:id)
    status, body, resp = req(port, "/items/sku-999", method="DELETE")
    check("DELETE /items/:id returns 404 (or 405 if unsupported)", True, status in (404, 405))

    # 8. Server-Sent Events (SSE) route
    status, body, resp = req(port, "/events", headers={"Accept": "text/event-stream"})
    check("GET /events status 200", 200, status)
    check("GET /events Content-Type", "text/event-stream; charset=utf-8", resp.getheader("Content-Type"))
    check("GET /events Cache-Control", "no-cache", resp.getheader("Cache-Control"))
    check("GET /events body event ping", True, "event: ping\ndata: pong" in body)
    check("GET /events body json data", True, 'data: {"status":"ok"}' in body)

    # 8b. Live SSE streaming route via writer object (fn (req, writer))
    t0 = time.time()
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    c.request("GET", "/stream/feed", headers={"Accept": "text/event-stream"})
    r = c.getresponse()
    check("GET /stream/feed status 200", 200, r.status)
    check("GET /stream/feed Content-Type", "text/event-stream; charset=utf-8", r.getheader("Content-Type"))
    chunk1 = r.readline().decode() + r.readline().decode() + r.readline().decode()
    check("GET /stream/feed received first event live", True, "event: progress" in chunk1 and '{"step":1}' in chunk1)
    rest = r.read().decode()
    stream_took = time.time() - t0
    check("GET /stream/feed streamed over time", True, stream_took >= 0.08)
    check("GET /stream/feed second event in rest", True, "event: progress" in rest and '{"step":2}' in rest)
    check("GET /stream/feed done event in rest", True, "event: done" in rest and "finished" in rest)
    c.close()

    # 8c. Live chunk/log-tail streaming via req.writer
    status, body, resp = req(port, "/stream/tail")
    check("GET /stream/tail status 200", 200, status)
    check("GET /stream/tail Content-Type", "text/plain; charset=utf-8", resp.getheader("Content-Type"))
    check("GET /stream/tail body", "log line 1\nlog line 2\n", body)

    # 9. Middleware short-circuiting: unauthorized access to /secure
    status, body, resp = req(port, "/secure")
    check("GET /secure without auth returns 401", 401, status)
    check("GET /secure without auth X-Middleware-Applied header", "1", resp.getheader("X-Middleware-Applied"))
    unauth_data = json.loads(body)
    check("GET /secure without auth error msg", "unauthorized", unauth_data.get("error"))

    # 10. Middleware success and request enrichment: authorized access to /secure
    status, body, resp = req(port, "/secure", headers={"Authorization": "Bearer secret"})
    check("GET /secure with auth returns 200", 200, status)
    check("GET /secure with auth X-Middleware-Applied header", "1", resp.getheader("X-Middleware-Applied"))
    auth_data = json.loads(body)
    check("GET /secure with auth user enriched", "admin", auth_data.get("user"))
    check("GET /secure with auth authorized true", True, auth_data.get("authorized"))

finally:
    server.terminate()
    server.wait()

if failed:
    sys.exit(1)
print("ALL ROUTE ERGONOMICS TESTS PASSED!")
