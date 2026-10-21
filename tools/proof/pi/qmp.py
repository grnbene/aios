import socket, json, sys, time
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/q.sock"); f = s.makefile("rw")
f.readline(); f.write(json.dumps({"execute":"qmp_capabilities"})+"\n"); f.flush(); f.readline()
def cmd(c, a=None):
    m = {"execute": c}
    if a: m["arguments"] = a
    f.write(json.dumps(m)+"\n"); f.flush(); return f.readline()
for arg in sys.argv[1:]:
    if arg.startswith("rel:"):
        _, dx, dy = arg.split(":")
        print(cmd("input-send-event", {"events":[{"type":"rel","data":{"axis":"x","value":int(dx)}},{"type":"rel","data":{"axis":"y","value":int(dy)}}]}).strip())
    elif arg.startswith("shot:"):
        print(cmd("screendump", {"filename": arg[5:]}).strip())
    elif arg.startswith("sleep:"):
        time.sleep(float(arg[6:]))
