"""Drives grain lsp as an editor would and prints what it answered (grain/tests/tools)."""
import json, subprocess, sys
p = subprocess.Popen([sys.argv[1], "lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
def send(obj):
    b = json.dumps(obj).encode()
    p.stdin.write(b"Content-Length: %d\r\n\r\n" % len(b) + b); p.stdin.flush()
def recv():
    n = 0
    while True:
        line = p.stdout.readline().strip()
        if not line: break
        if line.startswith(b"Content-Length:"): n = int(line.split(b":")[1])
    return json.loads(p.stdout.read(n))
uri = "file:///tmp/demo.gr"
send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})
r = recv(); print("initialize:", r["result"]["serverInfo"], sorted(r["result"]["capabilities"].keys()))
send({"jsonrpc": "2.0", "method": "initialized", "params": {}})
send({"jsonrpc": "2.0", "method": "textDocument/didOpen", "params": {"textDocument": {"uri": uri, "languageId": "grain", "version": 1, "text": "fn twice(x) {\nreturn x * 2\n}\nlet = 3\n"}}})
d = recv(); print("diagnostics:", [(x["range"]["start"]["line"], x["message"]) for x in d["params"]["diagnostics"]])
good = "fn twice(x) {\nreturn x * 2\n}\nlet total = twice(3)\n"
send({"jsonrpc": "2.0", "method": "textDocument/didChange", "params": {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": good}]}})
d = recv(); print("after the fix:", d["params"]["diagnostics"])
send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting", "params": {"textDocument": {"uri": uri}, "options": {"tabSize": 2, "insertSpaces": True}}})
r = recv(); print("formatting:", repr(r["result"][0]["newText"]))
send({"jsonrpc": "2.0", "id": 3, "method": "textDocument/documentSymbol", "params": {"textDocument": {"uri": uri}}})
r = recv(); print("symbols:", [(s["name"], s["kind"], s["range"]["start"]["line"]) for s in r["result"]])
send({"jsonrpc": "2.0", "id": 4, "method": "textDocument/completion", "params": {"textDocument": {"uri": uri}, "position": {"line": 0, "character": 0}}})
r = recv(); labels = [i["label"] for i in r["result"]]; print("completion:", len(labels), "items;", all(n in labels for n in ["print", "regex_match", "http_get", "twice", "total"]))
send({"jsonrpc": "2.0", "id": 5, "method": "textDocument/hover", "params": {}})
r = recv(); print("unknown request:", r["error"]["code"])
send({"jsonrpc": "2.0", "id": 6, "method": "shutdown"}); print("shutdown:", recv()["result"])
send({"jsonrpc": "2.0", "method": "exit"}); print("exit status:", p.wait(timeout=5))
