import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 42425)); f = open("/tmp/pi-udp42425.log", "a", buffering=1)
while True:
    d, a = s.recvfrom(65535); f.write(time.strftime("%H:%M:%S ") + a[0] + " " + d.decode("utf-8", "replace") + "\n")
