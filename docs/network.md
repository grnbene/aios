# Networking and network services

AIOS ships its own network code: a TCP/IP stack (`src/net/inet.cpp`), TLS 1.2/1.3
client and server (`src/net/tls*.cpp`), an SSH client (`src/net/ssh.cpp`), an
encrypted kernel-to-kernel link (`src/node/`), a fleet relay (`src/fleet/`), and
several services built on them (`src/channel/`, `tools/`). This page covers
how to run and use each one.

Everything below marked **driven** was run against the binaries in
`/repos/aios/build` on 2026-10-01, on 127.0.0.1, with `AIOS_NO_ACCEL=1` and
throwaway roots. The output shown is real, with some lines cut. Anything that

| You want to… | Use | Section |
|---|---|---|
| Serve a static site, an SPA, or PHP over HTTP/HTTPS | `aios-webserver <config>` | [Web server](#the-aios-web-server) |
| Fetch an `https://` page from the kernel | `sense <url>` (the TLS client) | [TLS](#tls-client-and-server) |
| Run a command on another machine over SSH | `ssh user@host[:port] <cmd>` | [SSH client](#the-ssh-client) |
| Link two kernels, use the other one's CPU/memory/channels, move an entity across | `node serve / connect / migrate` | [Node links](#node-links-between-two-kernels) |
| Many machines behind one rendezvous point | `fleet hub / join` | [Fleet relay](#the-fleet-relay) |
| Token-gated HTTP entry point for compute requests | `gateway serve` | [Gateway](#the-control-plane-gateway) |
| Let a non-aios process drive scoped agents | `aios-agent-control` | [Agent control](#the-agent-control-service) |
| Network from bare metal with no host OS | `host net up/ping/resolve/get` | [Native TCP/IP](#the-native-tcpip-stack-bare-metal) |
| Run any of these under systemd | `deploy/*.service` | [Running as a service](#running-as-a-service) |

---

## Which network stack is in use

The core never opens sockets itself. It goes through `aios::hal::net_listen` and
`net_connect` (`include/aios/hal/net.hpp`), and the substrate provides those:

- **Hosted (Linux, the normal `build/aios`)**: these are the host OS's sockets.
  Every service on this page runs over them. A connection waits for its
  next bytes in one blocking `recv` bounded by the socket's receive
  timeout, and answers with one `send`. `AIOS_NET_URING=1` drives the
  sockets through io_uring instead; measured on the request-and-answer
  shape of a SQL connection the plain path was faster on every workload
  (sysbench point lookups 185k/s against 165k/s, a tenth less CPU per
  query), so the ring is off unless asked for.
- **x86 bare metal (`metal/`)**: `net_connect` to a dotted IP or hostname goes
  through AIOS's own TCP/IP stack (`net::NetStack`, once `host net up` has run).
  `net_connect` to a MAC address, or to `any`, goes over the raw-Ethernet AOS
  *fabric* instead. `net_listen` listens only on the fabric
  (`metal/hal_metal.cpp:355`), so on metal a `node serve` is reached by MAC
  (`node connect <mac> <port> <token>`), not by IP.
- **arm64 bare metal (Pi)**: `metal/arm64/hal_net_arm64.cpp`. It takes dotted
  IPv4 only and has no resolver (`net_connect: '<host>' is not a dotted-decimal
  IPv4 address`).

See [the native TCP/IP stack](#the-native-tcpip-stack-bare-metal) for the metal
commands.

---

## The AIOS web server

`aios-webserver` (`tools/aios-webserver.cpp`) is a vhost-aware HTTP/HTTPS edge
server built to replace nginx in front of PHP-FPM. It terminates TLS itself and
picks the vhost from the SNI name, so one process can serve many domains on one
port. It serves static files directly and streams PHP requests to PHP-FPM over
FastCGI. Request and response bodies stream in chunks of at most 256 KiB; it
never holds a whole body in memory.

### Start it

```
aios-webserver <config-file>
```

The config file is the only argument. The server has no other flags and reads no
environment variables. It logs to stderr:

```
$ aios-webserver web.conf
aios-webserver: loaded TLS identity for localhost (1 cert(s) in chain)
aios-webserver: loaded TLS identity for rsa.local (1 cert(s) in chain)
aios-webserver: listening on :13430 (http, 1 vhost(s))
aios-webserver: listening on :13431 (https, 2 vhost(s))
```

It exits with status 1 if the config can't be read, has no `vhost` blocks, or a
TLS vhost's cert or key fails to load.

### Config file

The file is made of `vhost <server_name> { … }` blocks. `#` starts a comment.
Each directive goes on its own line:

| Directive | Meaning |
|---|---|
| `listen <port> [ssl] [loopback]` | Port to listen on. `ssl` turns on TLS. `loopback` binds 127.0.0.1 only (without it, the server binds all interfaces). |
| `cert <path>` | PEM certificate chain: leaf first, then intermediates. Required with `ssl`. |
| `key <path>` | PEM private key. Required with `ssl`. |
| `root <dir>` | Document root. |
| `index <file>` | Index and SPA fallback file. Default `index.html`. |
| `php <host:port>` or `php unix:/path.sock` | PHP-FPM FastCGI upstream. Without it the vhost serves static files only. |
| `max_body_size <bytes>` | Refuse a larger `Content-Length` with 413. Default 0, meaning unlimited. |
| `deny <4xx/5xx> <regex>` | Refuse paths that match (ECMAScript regex, case-insensitive, matched against the %-decoded, `//`-collapsed path) with that status. |
| `grain <script>` | Send requests that are not a static file (or PHP) to the script's `handle(req)` - see [Serving grain](#serving-grain). The script is relative to `root` or absolute. |
| `grain_ui <program.gr>` | Serve a grain `ui.pane` program as a web page: `GET` the page, `POST /_ui/event` for clicks and Enter, one session per browser (see [Serving a grain app](#serving-a-grain-app)). |
| `grain_native safe` | Allow `native fn` (the memory-safe kind) in the vhost's grain scripts. Off by default. |
| `grain_max_body <bytes>` | A request body up to this size is read before the handler runs and is `req.body`; a bigger or chunked one is streamed with `body_read()`. Default 1 MiB. `max_body_size` still bounds both (413). |
| `grain_steps <n>` | Instructions one grain request may run before it is stopped with a 500. Default 200000000. |

Vhosts that share a port share one listener. The **first vhost declared on a
port decides** that port's `ssl` and `loopback` settings
(`aios-webserver.cpp:510-511`).

Example (driven):

```
vhost localhost {
  listen 13430 loopback
  root /tmp/aios-docs-net/www
  index index.html
  deny 403 \.bak$
  max_body_size 1024
}
vhost localhost {
  listen 13431 ssl loopback
  cert /tmp/aios-docs-net/cert.pem
  key /tmp/aios-docs-net/key.pem
  root /tmp/aios-docs-net/www
}
vhost rsa.local {
  listen 13431 ssl loopback
  cert /tmp/aios-docs-net/rsacert.pem
  key /tmp/aios-docs-net/rsakey.pem
  root /tmp/aios-docs-net/www/sub
  index a.css
}
```

### Serving a directory (static / SPA)

This is what happens to each request (driven):

- If the path is a file under `root`, the server sends it with a MIME type from
  the extension (html, css, js/mjs, json, png, jpg, gif, svg, ico, woff/woff2,
  ttf, map, pdf, txt, xml, wasm; anything else gets `application/octet-stream`).
- **SPA fallback**: if the path is not a file, the server sends `root/index`. So
  `/no/such/route` returns 200 with the index page, not 404.
- **gzip**: applied when the client sends `Accept-Encoding: gzip`. The response
  is chunked because the compressed length isn't known in advance.
- **Range**: a single `bytes=` range returns 206 and is sent uncompressed.
- `HEAD` is supported.
- **Hidden paths**: any path segment starting with `.` returns 404, except under
  `/.well-known/`. This holds for `%2e` too, so a git checkout used as a root
  doesn't serve its `.git` directory. A path containing `..` returns 400 (or 404
  if the dot-segment rule catches it first).
- Keep-alive is honoured.

```
$ curl -si http://127.0.0.1:13430/
HTTP/1.1 200 OK
Content-Type: text/html
Accept-Ranges: bytes
Vary: Accept-Encoding
Content-Length: 35

<h1>hello from aios-webserver</h1>

$ curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:13430/.git/HEAD     -> 404
$ curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:13430/%2egit/HEAD   -> 404
$ curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:13430/old.bak       -> 403   (deny 403 \.bak$)

$ curl -s -D- -o /dev/null -H 'Accept-Encoding: gzip' http://127.0.0.1:13430/big.txt
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Encoding: gzip
Transfer-Encoding: chunked

$ curl -s -D- -o /dev/null -H 'Range: bytes=0-9' http://127.0.0.1:13430/big.txt
HTTP/1.1 206 Partial Content
Content-Range: bytes 0-9/6756
Content-Length: 10

$ curl -s -D- -o /dev/null --data-binary @big.txt http://127.0.0.1:13430/x     (max_body_size 1024)
HTTP/1.1 413 Payload Too Large
```

On plain HTTP the vhost comes from the `Host` header, and an unmatched name gets
the port's first vhost.

### Serving PHP (FastCGI to PHP-FPM)

Add `php` to a vhost. The routing rules (`handle_http_session`):

- A path ending in `.php` goes to PHP-FPM, with
  `SCRIPT_FILENAME = root + path`.
- A path that isn't a file goes to PHP-FPM as `/index` **only if `index` is a
  `.php` file** (a front controller). Otherwise it falls through to the static
  SPA fallback.
- Everything else is served as a static file.

The server sends the usual CGI parameters (`REQUEST_METHOD`, `QUERY_STRING`,
`REQUEST_URI`, `DOCUMENT_ROOT`, `REMOTE_ADDR`, `SERVER_NAME`, `HTTPS=on` on TLS
vhosts, `CONTENT_TYPE`/`CONTENT_LENGTH`, and one `HTTP_*` per request header).
PHP's `Status:` header sets the response code. The server drops `X-Powered-By`
and gzips PHP output by Content-Type unless PHP already set `Content-Encoding`.
If PHP-FPM can't be reached, the response is `502 upstream error: …`.

Driven, against a throwaway `php-fpm8.3` pool on 127.0.0.1:13438:

```
vhost php.local {
  listen 13439 loopback
  root /tmp/aios-docs-net/phpsite
  index index.php
  php 127.0.0.1:13438
}
```
```
$ curl -si 'http://127.0.0.1:13439/index.php?x=1'
HTTP/1.1 200 OK
X-From: php
Content-type: text/html; charset=UTF-8
Transfer-Encoding: chunked

php says hi; uri=/index.php?x=1 method=GET body=0 https=

$ curl -s http://127.0.0.1:13439/some/route              # front-controller fallback
php says hi; uri=/some/route method=GET body=0 https=
$ curl -s http://127.0.0.1:13439/static.html             # real file: served statically
<p>static beside php</p>
$ curl -s --data-binary @zero.bin http://127.0.0.1:13439/upload    # 3,000,000-byte body, streamed
php says hi; uri=/upload method=POST body=3000000 https=
```

`php unix:/run/php/pool.sock` is the Unix-socket form.

There's no generic HTTP reverse proxy: the only upstream protocol is FastCGI.

### Serving grain

A vhost with `grain app.gr` runs a site written in [grain](../grain/README.md).
A request for a file that exists is served as a file as before; anything else
goes to `handle(req)` in the script. The script's own source is never served.

```grain
fn handle(req) {
  if req.path == "/hello" { return text("hello " + req.query.name) }
  if req.method == "POST" { return json({"you_sent": req.form}) }
  if req.path == "/old"   { return redirect("/hello") }
  return {"status": 404, "headers": {"content-type": "text/plain"}, "body": "no route for " + req.path}
}
```

`req` is a map: `method`, `path`, `target`, `query_string`, `query` (a map; a
repeated name keeps its last value), `headers` (a map, names in lower case),
`body` (bytes), `text` (the body as a string), `form` (a map, when the body is
`application/x-www-form-urlencoded`), `ip`, `host`, `https`.

`handle` returns a string (200, `text/html`), bytes (200,
`application/octet-stream`), `nil` (204), or a response map `{status, headers,
body}`. A `body` that is a list or a map is sent as JSON. `text(s)`, `json(v)`
and `redirect(url)` build the common ones, each with an optional status
(`redirect` defaults to 302). A header name or value with a line break in it is
dropped, and the server sets `Content-Length` and `Connection` itself.

Each request runs the script in a fresh VM, so requests share nothing but what
they keep in files, and one that fails cannot affect another. A script may use
the standard library and the HTTP client; its files are confined to the vhost's
`root` (`read_file`, `write_file`, `list_dir`, ... take paths relative to it,
and `..` or an absolute path is an error). It has no sockets, processes,
windows, FFI or plugins. A script that fails to compile, raises an error,
returns something that is not a response, or runs past `grain_steps` gets a
500 whose cause goes to the log (stderr), never to the client.

With `grain_native safe`, `native fn` in the script compiles to machine code and
a handler can do its hot work at native speed (`native fn fib` is about 20x
the VM) on `bytes` and numbers only: no `ptr`, `extern fn`, FFI or plugins.
Native code is not counted by `grain_steps`, and native calls take turns, so
keep native functions short. Each recompile of an edited script keeps its
native code in memory until the server restarts.

A body larger than `grain_max_body`, or sent chunked, is not read up front:
`req.body`, `req.text` and `req.form` are `nil`, `req.streamed` is true, and the
handler pulls it with `body_read(n)`, which returns up to n bytes and an empty
`bytes` at the end. `body_read` works on small bodies too. The vhost's
`max_body_size` bounds the total either way (413), and a handler that leaves
part of a body unread gets its connection closed after the response.

The compiled script is kept and reused between requests (one is lent to one
request at a time, and the pool is dropped when the file changes), and a
response is gzipped when the client accepts it and it is at least 256 bytes.
`scripts/grain-web-check.sh` drives the whole thing.

### Serving a grain app

`grain_ui app.gr` serves a program that builds its screen with `ui.pane` (the
same program `grain app.gr` opens as a window and `grain serve app.gr` shows
on localhost) to browsers. A file that exists is served as a file; anything
else is the app's page, and `/_ui/event` takes its events. The program's source
is never served.

Each browser gets its own copy of the program, tied to it by an `HttpOnly`,
`SameSite=Strict` cookie (`Secure` over TLS). A click or Enter in the page posts
the fields' text and the action to the server, which calls the program's own
grain function and answers with the redrawn widgets; the page replaces them,
keeping the focused field and its caret. An event needs the header `X-Grain-UI: 1`, which a
page on another site cannot add, so cross-site requests cannot press the app's
buttons. The program's text is HTML-escaped, and the page carries a
`Content-Security-Policy` that allows only its own inline script and style.

The program runs with the standard library and the HTTP client and its files
confined to the vhost's `root`, as a `grain` script does, and each event is held to `grain_steps`
instructions. At most 256 sessions are kept (the least recently used goes) and
one idle for 30 minutes is dropped; a session whose program file changed starts
over on the new code. A failure is logged and the browser gets "internal server
error". Pages and events are gzipped when the client accepts it.

**Live data.** The program does not have to wait for a click to change: `every(1,
fn () { ... })` changes its data on a timer, and a pane made with `"shared": true`
is one copy for every visitor. A ticker in the server fires the program's timers
and re-renders; when the widgets differ from the last render every viewer is
sent the new ones - over a held connection (`text/event-stream`, chunked) to a
browser, which `EventSource` reconnects on its own, and by `?poll=N` (204 until
something changed) to a native client. Nothing in the program sends or listens. A
pane with `"refresh": n` is looked at every n seconds for views that read the
clock. A session with a viewer attached is never idle-dropped.

**Native clients.** `Accept: application/x-grain-ui` asks for the widgets as JSON
(`{"seq", "title", "width", "height", "tree"}`, a button carrying the `act` to post
back) instead of HTML; `grain open https://host/app.gr` draws them in a native
window with the same renderer a local program uses. Events post to the page's own
URL with `X-Grain-UI: 1`, and an action is named by where its widget sits and what
it says, so several viewers of one program never invalidate each other.

A pane drawn by hand (`create_pane` with `on_draw`) is sent as pictures: each
changed frame as a PNG down the same stream (up to ten a second while watched),
with the page's mouse, wheel and keys posted back as the pane's events. Not done: sessions are not shared between
server processes, and they are lost when the server restarts.

### TLS on the web server

To serve HTTPS, give a vhost `listen <port> ssl`, `cert` and `key`. When several
vhosts share an SSL port, the server reads the SNI from the ClientHello and
picks the vhost whose `server_name` matches. If the SNI is unknown or missing,
it uses the port's first vhost. Once TLS is up, the vhost is fixed by SNI and the
`Host` header is ignored.

Driven:

```
$ curl -v --cacert cert.pem --resolve localhost:13431:127.0.0.1 https://localhost:13431/
* SSL connection using TLSv1.3 / TLS_CHACHA20_POLY1305_SHA256 / X25519 / id-ecPublicKey
* ALPN: server accepted http/1.1
*  subject: CN=localhost
< HTTP/1.1 200 OK
<h1>hello from aios-webserver</h1>

$ curl -v --cacert rsacert.pem --resolve rsa.local:13431:127.0.0.1 https://rsa.local:13431/
* SSL connection using TLSv1.3 / TLS_CHACHA20_POLY1305_SHA256 / X25519 / RSASSA-PSS
*  subject: CN=rsa.local
< HTTP/1.1 200 OK
body{}
```

To make a test certificate:

```
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
  -keyout key.pem -out cert.pem -days 2 -subj /CN=localhost
```

For production, point `cert` at the full chain (e.g. Let's Encrypt
`fullchain.pem`) and `key` at its key.

See [TLS](#tls-client-and-server) for the supported protocols, suites and key
formats.

---

## TLS (client and server)

### Server (`net::TlsServer`, `src/net/tls.cpp`)

The server supports TLS 1.3, and falls back to TLS 1.2 for clients that can't do
1.3. Key exchange is x25519 only. It has no session resumption and doesn't take
client certificates.

Driven with `openssl s_client` against the web server:

| Client offers | Result |
|---|---|
| TLS 1.3 default | `TLS_CHACHA20_POLY1305_SHA256` |
| TLS 1.3 `TLS_AES_128_GCM_SHA256` | accepted |
| TLS 1.3 `TLS_AES_256_GCM_SHA384` only | handshake failure (alert 40) |
| TLS 1.2, ECDSA cert | `ECDHE-ECDSA-CHACHA20-POLY1305` |
| TLS 1.2, RSA cert, `ECDHE-RSA-AES128-GCM-SHA256` | accepted |
| TLS 1.2 `ECDHE-ECDSA-AES256-GCM-SHA384` only | handshake failure |
| TLS 1.3 with groups `P-256` only (no x25519) | handshake failure |

Key and certificate formats the loader accepts (driven):

| Key file | Result |
|---|---|
| PKCS#8 `BEGIN PRIVATE KEY`, EC P-256 | loads, serves |
| PKCS#8 EC P-384 | loads, serves |
| SEC1 `BEGIN EC PRIVATE KEY` | loads, serves |
| PKCS#1 `BEGIN RSA PRIVATE KEY` (2048) | loads, serves |
| PKCS#8 RSA | loads, serves |
| Ed25519 | `bad private key: invalid: unrecognised private key format (neither EC nor RSA)` |

`tools/tls-server-check.cpp` (`build/aios-tls-server-check`) is the self-test for
RSA/ECDSA signing and a live TLS 1.2 handshake. It uses fixed
ports 19488/19489, outside the range this run was allowed.

### Client (`net::TlsClient`)

`channel::http_send` uses the client for every `https://` URL. That covers
`sense`, the web pane, `git clone https://`, and the npm/pip/crates/go
fetchers. The client offers, in this order, TLS 1.3
`TLS_CHACHA20_POLY1305_SHA256`, `TLS_AES_128_GCM_SHA256`,
`TLS_AES_256_GCM_SHA384`, and TLS 1.2 ECDHE (ECDSA or RSA) with
ChaCha20-Poly1305, AES-128-GCM (SHA-256) or AES-256-GCM (SHA-384). Key exchange
is x25519 or secp256r1, whichever the server picks. A server that accepts only
AES-GCM, or only P-256, negotiates it. It verifies the server's handshake
signature (ECDSA P-256/P-384, RSA-PSS, RSA PKCS#1), then verifies the
certificate chain against a trust store and the hostname.

**This machine's own certificate.** When a server asks for a client certificate
(TLS 1.3 CertificateRequest, or TLS 1.2), the client presents `client.crt` and
signs with `client.key`, both beside the trust store (`<root>/ssl/`), or what
`TlsClient::use_client_certificate` was given. ECDSA P-256/P-384 and RSA keys
work. With no certificate on file it sends an empty Certificate and the server
decides. Driven against `openssl s_server -Verify 1`:

```
$ $A --root $R exec "sense https://localhost:13583/index.html"      # TLS 1.3
  from          : https://localhost:13583/index.html over TLS 1.3 TLS_CHACHA20_POLY1305_SHA256 — trusted via test-ca (our certificate presented)
$ $A --root $R exec "sense https://localhost:13577/index.html"      # TLS 1.2
  from          : https://localhost:13577/index.html over TLS 1.2 ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256 — trusted via test-ca
$ $A --root $R exec "sense https://localhost:13591/index.html"      # no client.crt on file
FAILED: io-error: the server requires a client certificate and this machine presented none (put client.crt and client.key beside its trust store)
```

**Session resumption (TLS 1.3).** A NewSessionTicket the server sends after the
handshake is kept for the process, one per `host:port`, and offered on the next
connection to that server (`pre_shared_key` with `psk_dhe_ke`, so the session
still has fresh key exchange). The server's identity is the one proven in the
session the ticket came from; a resumed handshake carries no certificates.
Tickets are single use and honour the server's lifetime. Driven against
`openssl s_server`, two fetches in one process:

```
  from          : https://localhost:13584/index.html over TLS 1.3 TLS_CHACHA20_POLY1305_SHA256 — trusted via test-ca
  from          : https://localhost:13584/index.html over TLS 1.3 TLS_CHACHA20_POLY1305_SHA256 — trusted via test-ca (session resumed)
```
(`openssl s_server -www` reports the second as `Reused, TLSv1.3`.)

**Trust store**: the client uses the first of these that exists
(`default_trust_store`):

1. `<root>/ssl/ca-certificates.crt`, the kernel's own store
2. `/etc/ssl/certs/ca-certificates.crt`
3. `/etc/pki/tls/certs/ca-bundle.crt`
4. `/etc/ssl/cert.pem`
5. `/usr/local/etc/ssl/cert.pem`

To make one kernel trust a private CA or a self-signed certificate, put the PEM
at `<root>/ssl/ca-certificates.crt`. Once that file exists the client uses it
*instead of* the system bundle, not alongside it.

Driven, with the kernel's TLS client talking to the AIOS web server:

```
$ mkdir -p rootA/ssl && cp cert.pem rootA/ssl/ca-certificates.crt
$ aios --root rootA exec 'sense https://localhost:13431/ keep=0'
[SENSE] https://localhost:13431/
  from          : https://localhost:13431/ over TLS 1.3 TLS_CHACHA20_POLY1305_SHA256 — trusted via localhost
  read          : 35 B on the wire, 25 B of text, in 156.01 ms
  | hello from aios-webserver

$ aios --root rootA exec 'sense https://127.0.0.1:13431/ keep=0'
FAILED: denied: the certificate is for localhost, not 127.0.0.1

$ aios --root rootC exec 'sense https://localhost:13431/ keep=0'      # system bundle only
FAILED: denied: the chain ends at localhost, issued by localhost, which is not in this machine's trust store
```

---

## The SSH client

`ssh` (`cmd_ssh`, `src/net/ssh.cpp`) runs **one command** on a remote host and
prints its output and exit status. There's no interactive terminal.

```
ssh <user@host[:port]> <command...> [key=<path>] [passphrase=@FILE|env:NAME] [as=$entity]
ssh keygen [path]          # create (or show) this kernel's identity
ssh known                  # list recorded host keys
```

You must give a user (`user@host`). `ssh://user@host:port` also works. Running
`ssh` needs the network capability on the acting entity.

**What it negotiates**: kex `curve25519-sha256` (or `@libssh.org`), host key
`ssh-ed25519`, `rsa-sha2-512` or `rsa-sha2-256`, and cipher
`chacha20-poly1305@openssh.com`. The user key given with `key=` may be
ed25519, RSA or ECDSA (P-256/384/521), each signing under its own algorithm
name; an ECDSA key used to be offered as `ssh-ed25519` and refused by every
server, where ed25519 and RSA worked. Driven against OpenSSH 9.6: all three
log in, a key that is not in `authorized_keys` is refused. The host's signature must use the algorithm the
exchange agreed on. SHA-1 `ssh-rsa` signatures are not offered or accepted, as
OpenSSH itself stopped accepting them in 8.8.

**Keys it accepts**: OpenSSH-format (`-----BEGIN OPENSSH PRIVATE KEY-----`)
Ed25519 and RSA keys, unencrypted or encrypted with a passphrase
(`aes128-ctr`/`aes192-ctr`/`aes256-ctr` with `bcrypt` key derivation, what
`ssh-keygen -N` writes), and PKCS#1 or PKCS#8 PEM RSA keys. An RSA identity signs
with `rsa-sha2-512` where the server lists it in `server-sig-algs`, else
`rsa-sha2-256`. A passphrase comes from `passphrase=@FILE` (its first line),
`passphrase=env:NAME`, or `AIOS_SSH_PASSPHRASE`; the passphrase itself on the
command line is refused, because the line is kept in the console transcript.
A legacy encrypted PEM key (`Proc-Type: 4,ENCRYPTED`) is refused with the
`ssh-keygen -p` command that rewrites it. The default identity is
`<root>/ssh/id_ed25519`, created on first use from the machine's randomness, with
a `.pub` beside it. `key=<path>` uses a different key.

**Host keys**: trust on first use, stored in `<root>/ssh/known_hosts`. A changed
key is refused.

Driven against an unprivileged OpenSSH 9.6 `sshd` on 127.0.0.1:2299 whose only
host key is RSA (`HostKeyAlgorithms rsa-sha2-512,rsa-sha2-256`), with an RSA key
made by `ssh-keygen -t rsa -b 3072 -N '<passphrase>'`:

```
$ aios --root sshroot exec 'ssh arch@127.0.0.1:2299 echo hi from sshd key=user_rsa passphrase=@pp'
ssh arch@127.0.0.1:2299
  server        : SSH-2.0-OpenSSH_9.6p1 Ubuntu-3ubuntu13.19
  host key      : SHA256:uEQvHF7FU89IchxhmNeqd/4wF3aKFHk+YF4SmCwOYNQ  (first time — recorded)
  negotiated    : curve25519-sha256 + chacha20-poly1305@openssh.com
  identity      : SHA256:b6PTXNNEUX3i/EO5cq2hv9xwAEM+AippTcfvjGw30eA
  $ echo hi from sshd
  hi from sshd
  exit          : 0   3028 bytes in, 1412 bytes out
```

The recorded host key is the server's (`ssh-keygen -lf host_rsa.pub` prints the
same fingerprint). The same run with an encrypted Ed25519 key and
`passphrase=env:PP` logs in too. Without a passphrase, with a wrong one, and with
the passphrase written on the line:

```
FAILED: denied: user_rsa: private key is encrypted with aes256-ctr/bcrypt; it needs its passphrase (passphrase=@FILE or env:NAME, or AIOS_SSH_PASSPHRASE)
FAILED: denied: user_rsa: incorrect passphrase or corrupted private key (check words differ)
FAILED: invalid: passphrase= takes @FILE or env:NAME, not the passphrase itself (it would be kept in the transcript)
```

A key the server does not list:

```
FAILED: denied: the server refused this identity for arch@127.0.0.1 (SHA256:78PFZRPcWjLAuHKAlpahhqKt7+1DGd+FF5z+gtHunUg); it will accept: publickey
```

Driven against a throwaway, unprivileged OpenSSH `sshd` on 127.0.0.1:13441, with
its own host key and `AuthorizedKeysFile`:

```
$ aios --root sshroot exec 'ssh keygen'
[SSH KEYGEN] ed25519 identity of kernel 3803a4266ff2 at sshroot/ssh/id_ed25519 (generated from this machine's randomness)
  fingerprint   : SHA256:cZiI8EwSfvUhU8Fu8zHMeD0JGqIeTUcmco5lnPVBtOs
  public key    : ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIIbt...S0Wr aios-3803a4266ff2
  put that line in the remote account's authorized_keys and this machine can reach it

$ aios --root sshroot exec 'ssh arch@127.0.0.1:13441 uname -s; echo hi from sshd'
ssh arch@127.0.0.1:13441
  server        : SSH-2.0-OpenSSH_9.6p1 Ubuntu-3ubuntu13.19
  host key      : SHA256:UMAuKm57PKrUdiwhSD9KyrHEwMXN1m5JZ/a+8cug9hk  (first time — recorded)
  negotiated    : curve25519-sha256 + chacha20-poly1305@openssh.com
  identity      : SHA256:cZiI8EwSfvUhU8Fu8zHMeD0JGqIeTUcmco5lnPVBtOs
  $ uname -s; echo hi from sshd
  Linux
  hi from sshd
  exit          : 0   1960 bytes in, 724 bytes out

$ aios --root sshroot exec 'ssh known'
Hosts this machine has met (sshroot/ssh/known_hosts)
  [127.0.0.1]:13441           ssh-ed25519     SHA256:UMAuKm57PKrUdiwhSD9KyrHEwMXN1m5JZ/a+8cug9hk
```

Failure cases (driven):

```
key=user_enc (passphrase)  FAILED: unsupported: …/user_enc: private key is encrypted with aes256-ctr/bcrypt — this machine reads an unencrypted key
key=user_rsa               FAILED: unsupported: …/user_rsa: key type ssh-rsa — this machine signs with ssh-ed25519
remote `exit 3`            exit : 3 …   then  FAILED: io-error: the remote command exited 3
no user                    FAILED: invalid: no user for 127.0.0.1:13441 (use user@host)
RSA-only server            FAILED: unsupported: no host key algorithm in common with SSH-2.0-OpenSSH_9.6p1 … (it offers rsa-sha2-512,rsa-sha2-256)
host key changed           FAILED: denied: the host key for [127.0.0.1]:13441 has CHANGED — this machine knows a different key for it (SHA256:pEi6… now). Remove the line from sshroot/ssh/known_hosts if the host was legitimately rebuilt.
```

To let a kernel reach an account, add the `public key` line from `ssh keygen` to
that account's `~/.ssh/authorized_keys`.

---

## Node links between two kernels

A node link joins two AIOS kernels. The kernel that runs `node serve` accepts
links. Each kernel that connects to it becomes a **guest entity** there, and the
server checks every request against that guest's capabilities, the same way it
checks a local entity's. On the connecting kernel, the remote kernel shows up as
a device (`remote:<host>:<port>/cpu`), as a remote memory provider, and as a
channel transport.

```
node serve <port> <token>                 # accept peers (this kernel serves)
node connect <host> <port> <token>        # attach a peer
node list                                 # peers this kernel attached (client side)
node peers                                # guests attached to us (server side)
node migrate <entity> [node-index=0] [keep]
```

**Security**: the link is encrypted with ChaCha20-Poly1305
(`src/node/secure.cpp`). Each side derives a key from the shared token with a
fixed salt, and proves it knows the token in a challenge exchange. A fresh
session key comes from both sides' random nonces. Frames carry a sequence number
in the nonce and AAD, and a replayed or reordered frame kills the link. **The
token must be at least 16 characters.** `node serve` listens on **all
interfaces**; it has no loopback option, so limit access with a firewall.

**Link compression**: during the initial secure handshake (`src/node/secure.cpp`),
nodes advertise supported transport features (`link_feature_payload()`, feature
word containing `kFeatDeflate`). When both peers advertise compression support
(`link_peer_deflates()`), the link frame stream is transparently compressed with
deflate (`Link::set_compress(true)`), saving bandwidth across WAN for inter-node
RPC, migration payloads, and relay command streams.

### Driven: two kernels, two processes

Two `aios shell` processes ran on separate roots (`nA`, `nB`), driven through a
pty. This is the same method as `scripts/drive-migrate.py`. `B>` is the serving
kernel; `A>` connects.

```
B> node serve 13432 docs-node-token-123
   serving this kernel on port 13432 (peers act here as guest entities; link authenticated and encrypted with the token)
A> node connect 127.0.0.1 13432 wrong-token
   error: invalid: node token must be at least 16 characters
A> node connect 127.0.0.1 13432 docs-node-token-123
   attached node 127.0.0.1:13432: 1 device(s) [remote:127.0.0.1:13432/cpu], provides remote memory, transport network:127.0.0.1:13432
A> node list
   127.0.0.1:13432 kernel 93915c649fe0 calls=0 devices=1
B> node peers
   guest ef598668341069d6000d0bd4f1107dc6 from 127.0.0.1:59846 kernel b47d35523bdf requests=0
A> devices
   DEVICE ID     TYPE      LANES   BUSY    MEMORY        NATIVE    NAME
   d5b5be9bba95  cpu       4       0       19.49 GiB     system    cpu
   a035fb5ec72f  remote-cpu4       0       19.49 GiB     remote    remote:127.0.0.1:13432/cpu
```

A wrong token of valid length (driven separately):

```
$ aios --root nD exec 'node connect 127.0.0.1 13433 this-is-the-wrong-token'
FAILED: denied: node: node token rejected
```

**Remote memory and a remote channel.** `node connect` sets `$transport`, which
`channel create … transport=$transport` uses:

```
A> entity create worker                      => $w
A> cap grant $w compute * execute,schedule
A> cap grant $w kernel * schedule,checkpoint
A> cap grant $w network * publish
A> mem alloc $w 64 remote
   memory 304123c55ece3e85000d0a59e742b6e0 (64 B remote)
A> mem write $w $rm 0 "bytes held on node B"
   wrote 20 bytes
A> mem read $w $rm 0 20
   62797465732068656c64206f6e206e6f64652042  "bytes held on node B" (20 bytes)
A> channel create $w 8 512 transport=$transport
   channel de9bb628dcadb2c2000d0a59e772b6e0 (8 frames × 512 B, transport network:127.0.0.1:13432, region 7cd030565f63)
A> state create $w working "published from node A"    => $st
A> channel publish $w $ch $st
   published frame #0
A> channel consume $w $ch
   frame #0 from 02fd7111449a state 1b2a82a5dd42 (working gen=0, 0 memory refs)  "published from node A" (21 bytes)
```

To run a graph op on the peer, give it `dev=remote-cpu`; see
`scripts/drive-node-a.aos`.

### Migrating a running entity

`node migrate <e> [idx]` checkpoints the entity: its states, memory, capabilities
and any execution in progress. It ships that checkpoint to the peer at index
`idx` in `node list` and restores it there with the same entity ID. Then it
removes the entity locally. With `keep`, the original stays and the peer gets a
clone.

Driven: a three-op graph runs one op on A, moves to B, and finishes on B.

```
A> graph op $w $g matmul in=$A:f32:2x3 in=$B:f32:3x2 out=$C:f32:2x2
A> graph op $w $g relu in=$C:f32:2x2 out=$D:f32:2x2
A> graph op $w $g softmax in=$D:f32:2x2 out=$D:f32:2x2
A> graph run $w $g 1
   execution eb39cb2054e0: ran 1/3 ops, position 1 (resumable) in 4.98 ms
A> node migrate $w 0
   migrated 02fd7111449a to peer node #0 — it lives there as 02fd7111449a5fdd000d0a59e6c2b6e0 and is gone here
A> entity list
   b47d35523bdf  kernel ...   07887dd8b57e  console ...   9797a6474f1c  mind ...      (worker is gone)
B> entity show 02fd7111449a5fdd000d0a59e6c2b6e0
     label=worker status=active gen=25 states=2 memory=6 caps=13 checkpoints=0 restores=1 execution=eb39cb2054e0@1
B> graph run 02fd7111449a5fdd000d0a59e6c2b6e0 eb39cb2054e0e9ab000d0a59e822b6e0
   execution eb39cb2054e0: ran 2/3 ops, position 3 (complete) in 71.4 µs
B> tensor show 02fd7111449a5fdd000d0a59e6c2b6e0 aea6fe97564c1181000d0a59e802b6e0 f32 2x2
   f32[2x2]@aea6fe97564c = [0.0024726 0.99753 3.059e-07 1]
```

`node serve` returns as soon as it starts listening. In `aios exec` mode, keep
the process alive with something after it, such as
`aios exec 'node serve 13433 <token>' 'sleep 600'`, or run it inside a `shell`.

---

## The fleet relay

With direct node links, each kernel needs every peer's address. The fleet
overlay (`src/fleet/relay.cpp`) avoids that. One kernel runs a **hub**, and
members attach to it with a shared **cluster key**. The hub keeps the roster and
switches byte circuits between members. Every circuit carries the same
token-authenticated, end-to-end-encrypted node handshake used by direct links,
so the hub only ever sees ciphertext. Members heartbeat every 6 s. The hub
evicts a member it hasn't heard from in 20 s and re-announces the roster.

```
fleet hub <port> <key|@FILE|env:NAME> [bind=127.0.0.1]   # become the rendezvous + relay
fleet join <hub-host> <hub-port> <key|@FILE>   # attach as a member (also serves peers' SQL reads)
fleet members                                  # roster, self excluded
fleet status                                   # load of this kernel and each peer
fleet run <peer-id-prefix> <command...|@FILE>  # run a command on a peer
fleet observe <hub> <port> <key> [status-file] [interval-ms]   # read-only, writes JSON (default /opt/aos/fleet-status.json)
fleet plan | tick | watch [ticks] [ms] | slots [n] | watermarks <hi> <lo> | replicate [idx] | replicas | log | db
```

`@FILE` reads the key from the first line of a file and `env:NAME` from an
environment variable; both keep it out of `ps`. The key must be at least 16
characters. The hub listens on all interfaces unless given `bind=127.0.0.1`.
A member id already held by a live session (heard from in the last 12 s)
cannot be taken over by a second attach; the newcomer is refused.

Driven: a hub and two members in three processes.

```
HUB> fleet hub 13434 short
   error: invalid: cluster key must be at least 16 characters
HUB> fleet hub 13434 docs-fleet-cluster-key-01
   fleet hub up on port 13434 — members attach here; it keeps the roster and relays circuits, seeing only ciphertext
A> fleet join 127.0.0.1 13434 docs-fleet-cluster-key-01
   joined the fleet via 127.0.0.1:13434 — roster: 0 peer(s) discovered
B> fleet join 127.0.0.1 13434 docs-fleet-cluster-key-01
A> fleet members
   Fleet roster (discovered via the hub, self excluded)
     6d1d4fc5d56f
A> fleet status
   Fleet load  (high-water 1.00, low-water 0.75)
     * this  256da4b725cd  jobs 0/4 slots  pressure 0.44  free 4  (4 cpus)
     (in a fleet; 1 peer(s) in the roster, reached on the next tick)
A> fleet run 6d1d4fc5d56f info
   error: unsupported: node relay:6d1d4fc5d56f: this node does not take commands
A> node list
   relay:6d1d4fc5d56f kernel 6d1d4fc5d56f calls=1 devices=1
```

`join` reports `roster: 0` because the roster arrives a moment after attach;
`fleet members` shows it. `fleet run` reaches the peer over the relay, but only
a kernel that installs a command runner answers it. Today that's only the arm64
Pi fleet shell (`metal/arm64/fleet_shell.cpp:207`). Once a peer is reached over
the relay it appears in `node list` as `relay:<id>`, and `node migrate` works
the same way on it.

A wrong cluster key (driven):

```
$ aios --root fY exec 'fleet join 127.0.0.1 13435 the-wrong-cluster-key'
FAILED: io-error: could not attach to hub 127.0.0.1:13435 (unreachable, or wrong cluster key)
```

The orchestrator (`src/fleet/orchestrator.cpp`) uses the same links for
placement. `plan` shows what it would do, `tick` and `watch` execute
migrations between peers above the high watermark and below the low one, and
`replicate` ships recovery points to a peer. [fleet.md](fleet.md) shows them
moving work off an overloaded member and restoring it on a standby.

---

## The control-plane gateway

`gateway serve` (`src/channel/gateway.cpp`) is an HTTP/1.1 and length-framed JSON
entry point. It checks an HMAC-signed **capability token** on each request and
returns a **routing decision** for it: admit locally, route to a peer, rebalance,
or reject. It doesn't run the job itself; the decision says where the job should
go.

```
gateway serve [port=7310] [public=1] [secret=S] [tls=1 cert=PATH key=PATH] [cors=ORIGIN] [require_custom_secret=1] [max_depth=N]
gateway token [holder=<entity>] [scope=100] [duration_s=3600] [secret=S]
gateway route <req.json|json-string> [token=T] [secret=S]
gateway status
```

- `public=1` binds 0.0.0.0. The default is 127.0.0.1.
- **Secret**: this is the HMAC key that signs and checks tokens. Every
  subcommand resolves it the same way: `secret=` if given, else
  `AIOS_GATEWAY_SECRET`, else the root's own key in `<root>/gateway/secret`.
  There is no built-in secret. The root's key is 32 random bytes (64 hex
  characters), made the first time it is needed, in an owner-only directory
  (0700, the file 0600), and reused after that. `serve` prints its path when it
  uses it. So `gateway token` run on the same root as the server mints tokens the
  server accepts; to call the server by hand, send the file's contents as
  `X-Gateway-Secret`. A file that is not 64 hex characters is refused; remove
  it to make a new key.
  `require_custom_secret=1` refuses the root's key and needs `secret=` or
  `AIOS_GATEWAY_SECRET`: gateways that must accept each other's tokens need one
  shared secret, and each root's own key differs.
- `tls=1` serves HTTPS with the tree's own TLS server (`net::TlsServer`, the one
  `aios-webserver` uses): TLS 1.3 or 1.2, x25519, ChaCha20-Poly1305 or
  AES-128-GCM. `cert=` is a PEM chain and `key=` a PEM private key (EC or RSA).
  Both are loaded at startup; a missing or unparseable file is a startup error,
  never a fall back to plaintext.
- `cors=ORIGIN` names the one browser origin allowed to read answers. With no
  `cors=`, no `Access-Control-Allow-Origin` header is sent. `cors=*` is refused.

**Who may call what.** The operator proves itself with the secret, sent as
`X-Gateway-Secret: <secret>` (over `tls=1` when off loopback). A client proves
itself with an unexpired capability token signed by the secret, sent as
`Authorization: Bearer <token>`, `X-Capability-Token: <token>`, or a JSON
`"token"` field.

| Method, path | Needs | Does |
|---|---|---|
| `GET /`, `/status`, `/health` | nothing | Load summary JSON |
| `GET /fleet`, `/telemetry` | token or secret | Local node, peers, and the orchestrator's recorded migrations |
| `GET /checkpoints` | token or secret | IDs and timestamps in the object store |
| `GET /query?q=<AQL>`, `POST /query {"query":…}` | token or secret | AQL query over the object store |
| `GET/POST /token`, `/mint_token` | secret | Mints a token (default scope 100, 1 hour; a POST body may set `scope`, `duration_s`) |
| `POST /rebalance` | secret | Runs an orchestrator rebalance |
| `POST /rewind {"path":…}` | secret | Rewinds to a checkpoint |
| `POST /route` (any other POST) | token | Routing decision. 403 for unauthorized, expired or scope exceeded; 429 when every capable node is saturated; 400 `RejectedUnsupportedOp` when no node runs the op. |

Missing credentials answer 401; a token on a secret-only endpoint answers 403. A
`GET` to any other path answers 404.

`/fleet` reports, per peer, its id, address, `reachable`, pressure, queued jobs
and free slots. Peer telemetry carries no architecture, so only the local node
has an `arch`. Each migration is the orchestrator's own record: `kind`
(rebalance/recover), `from`, `to`, both pressures, `status`, `ns` and `reason`.

The cost of a request is 1, or for `tsp_exact`/`route_improve` the number of
stops (+1 for `tsp_exact`). A token's `scope` caps that cost.

Driven (ports 13560-13561, self-signed `cert.pem`/`key.pem` for `localhost`):

```
$ aios exec 'gateway serve port=13560 require_custom_secret=1'
FAILED: denied: require_custom_secret=1: set secret= or AIOS_GATEWAY_SECRET (this root's own key is not used)
$ aios exec 'gateway serve port=13560 tls=1 cert=/nonexistent key=/nonexistent'
FAILED: not-found: tls cert /nonexistent: /nonexistent: No such file or directory
$ aios --root R exec 'gateway serve port=13560 public=1'
starting gateway server on port 13560 (loopback_only=0, tls=0)...
token secret: this root's own key, R/gateway/secret
control plane gateway listening on 0.0.0.0:13560 [plain HTTP/1.1 + wire]

$ AIOS_GATEWAY_SECRET=envsecret-123 aios exec 'gateway serve port=13560 tls=1 cert=cert.pem key=key.pem cors=https://ops.example.com'
control plane gateway listening on 127.0.0.1:13560 [HTTPS: TLS 1.3/1.2, cert localhost] cors=https://ops.example.com
$ curl --cacert cert.pem https://localhost:13560/health
{"status":"ok","gateway":"aios-control-plane","local_pressure":0.491002,...}   [200]
$ openssl s_client -connect 127.0.0.1:13560 -tls1_3 -ciphersuites TLS_AES_128_GCM_SHA256
New, TLSv1.3, Cipher is TLS_AES_128_GCM_SHA256

$ curl .../token                                   -> 401 needs the gateway secret
$ curl .../query  ;  .../fleet  ;  .../checkpoints -> 401 unauthorized
$ curl -X POST .../rebalance                       -> 401;  with a token only -> 403
$ curl -X POST -H 'X-Gateway-Secret: envsecret-123' -d '{"scope":10,"duration_s":60}' .../token
{"ok":true,"token":"426041e6…"}
$ curl -X POST -H 'X-Gateway-Secret: envsecret-123' .../rebalance
{"ok":true,"rebalances":0}   [200]
$ curl -H 'Origin: https://ops.example.com' -i .../health | grep Access-Control-Allow-Origin
Access-Control-Allow-Origin: https://ops.example.com      (any other Origin: no CORS headers)

# the CLI mints with the same secret the server uses
$ TOK=$(AIOS_GATEWAY_SECRET=envsecret-123 aios exec 'gateway token scope=10 duration_s=60' | tail -1)
$ curl -X POST -H "Authorization: Bearer $TOK" -d '{"op":"erp.tsp_exact","stops":[1,2]}' .../route
{"ok":true,"status":"AdmittedLocal",...,"cost_units":3,...}   [200]
$ curl -X POST -H "X-Capability-Token: $TOK" -d '{"op":"erp.tsp_exact","stops":[1,…,11]}' .../route
{"ok":false,"status":"RejectedScopeExceeded",...,"reason":"resource cost units exceed token scope limit (12 > 10)"}   [403]
$ curl -X POST -H "X-Capability-Token: $TOK" -d '{"op":"matmul"}' .../route
{"ok":false,"status":"RejectedUnsupportedOp",...,"reason":"no node in the fleet has op 'matmul' registered"}   [400]
```

`gateway status` lists the custom kernels registered on this machine
(`erp.tsp_exact, erp.route_improve, erp.pool_greedy`). An op that no node
(this one, or a peer that reported it among its kernels) has registered is
`RejectedUnsupportedOp`; `RejectedOversubscribed` (429) is only for an op some
node runs while every such node is above its headroom.

### Compute service (`compute serve`)

The gateway's unit orders itself after `aios-compute.service`, which runs
`compute serve` (`src/channel/compute_serve.cpp`). That service speaks
length-framed JSON (a 4-byte big-endian length, then the JSON) on port 7311.
`127.0.0.1` is the default; `public=1` binds all interfaces. Options:
`port= lanes= max_points= token= token_file= public=1 shm_ring= sql=1 …`.

Driven, with `compute serve port=13444 token=docs-compute-token`:

```
compute service listening on 127.0.0.1:13444 lanes=4 max_points=15 token=required
{"op":"ping"}                         -> {"ok":false,"op":"ping","error":"unauthorized"}
{"op":"ping","token":"…"}             -> {"ok":true,"op":"ping","kernel":"1362fa53cde8",...,"ops":["ping","stats","tsp_exact","route_improve","pool_greedy"],"max_exact_points":15}
{"op":"tsp_exact","token":"…",start,3 stops} -> {"ok":true,"status":"done","order":["-1","a","c","b"],"distance_miles":13.623946,...}
```

---

## The agent-control service

`aios-agent-control` (`tools/aios-agent-control.cpp`,
`src/channel/agent_control_serve.cpp`) lets a non-aios process create
capability-scoped agent entities, bind a mind to them, grant/attenuate/revoke
authority, run perceive→think→act cycles, and read the authority ledger.

```
aios-agent-control [--root DIR] [--port 7312] [--listen-all] [--auth-secret-file FILE]
```

- The default root is `/tmp/aios-agent-control-data` and the default port 7312.
  It binds 127.0.0.1 unless you pass `--listen-all`, which is refused without
  `--auth-secret-file`.
- `AIOS_AGENT_CONTROL_MEMORY_MB` (default 256) sets the memory quota the service
  can hand to agents.
- **Wire format**: a 4-byte big-endian length, then a JSON object; one request
  and one response per connection.
- **Auth**: with `--auth-secret-file`, every request must carry
  `"ts"` (unix seconds), `"nonce"` (8-128 characters, unique) and
  `"auth": hex(HMAC-SHA256(secret, <request JSON without "auth">))`, so ts and
  nonce are signed. The service accepts a ts within 300 s of its clock and each
  nonce once inside that window: a captured request replayed later is refused
  (`unauthorized: replayed nonce`, or `stale or future ts`). Trailing newlines in
  the secret file are stripped. Without a secret file no auth is checked, and
  the service then only listens on loopback.
- `agent.bind_mind` takes the endpoint from the request's `"url"`, else from
  `AIOS_MIND_URL` in the service's environment; with neither it answers an
  error. No endpoint is built in.
- Ops: `svc.ping`, `agent.create`, `agent.bind_mind` (kind `qwen` or `external`),
  `agent.send`, `agent.terminate`, `agent.list`, `cap.grant`, `cap.request`,
  `cap.revoke`, `cap.exercise`, `cycle.run`, `cycle.run_n`, `ledger.events`,
  `entity.status`.
- `--external-mind NAME=COMMAND` (repeatable) registers an out-of-process mind
  (the AOSX frame protocol, e.g. mcp-tools' `aios/intelligence/assay/bridge.cjs`).
  `agent.bind_mind {"kind":"external","mind":"NAME","provider":"...","model":"..."}`
  spawns it for one entity; provider and model reach it as `AIOS_MODEL_PROVIDER` /
  `AIOS_MODEL`. A caller can only pick a registered name, never name a program.
- `cap.exercise {"entity","kind","resource","rights","action"}` is the check a broker
  makes before carrying out an action for an entity outside the kernel (editing a
  file, running a command). Allowed: an `exercised` ledger event naming the action;
  denied: `REFUSED` with the reason. Requests that take `resource_id` also take
  `resource`, a name hashed to an id the same way everywhere, so a grant over
  `path:/w/a/README.md` and an exercise over that name meet.
- The service holds `storage:*` read/write/grant and `network:*` publish/consume/grant
  only to hand narrowed copies to agents. A grant over one named resource covers that
  resource alone: `delegate` now takes the narrower resource (before, a capability
  derived from a class capability silently covered the whole class).

Driven (port 13437):

```
$ printf 'docs-agent-control-secret\n' > ac.key
$ aios-agent-control --root acroot --port 13437 --auth-secret-file ac.key
  [ONLINE] agent-control resident: 837e766d5a48d0c9000f256da2981489
  Authority held:
    state:* : read,write,grant,destroy,allocate
    kernel:* : read,grant,schedule
    memory:* : read,write,grant,destroy,allocate (scope 268435456)
    compute:* : execute,grant,schedule
    entity:* : read,execute,grant,destroy,allocate
    channel:* : grant,destroy,publish,consume,allocate
[wire] agent-control listening on 127.0.0.1:13437 auth=required
```

A Python client:

```python
import socket, struct, json, hmac, hashlib, time, os
SECRET = b"docs-agent-control-secret"
def call(req):
    req = dict(req, ts=int(time.time()), nonce=os.urandom(12).hex())
    body = json.dumps(req, separators=(",", ":"))
    req = dict(req, auth=hmac.new(SECRET, body.encode(), hashlib.sha256).hexdigest())
    body = json.dumps(req, separators=(",", ":")).encode()
    s = socket.create_connection(("127.0.0.1", 13437))
    s.sendall(struct.pack(">I", len(body)) + body)
    n = struct.unpack(">I", s.recv(4))[0]; d = b""
    while len(d) < n: d += s.recv(n - len(d))
    return json.loads(d)
```
```
unsigned svc.ping  -> {'ok': False, 'op': 'svc.ping', 'id': '1', 'error': 'unauthorized'}
svc.ping           -> {'ok': True, 'op': 'svc.ping', 'id': '1'}
agent.create       -> {'ok': True, 'op': 'agent.create', 'id': '2', 'entity': '76e2227fd7423464000f256da3081489'}
agent.list         -> {'ok': True, 'op': 'agent.list', 'id': '3', 'agents': [{'entity': '76e2…1489', 'label': 'docs-agent'}]}
agent.terminate    -> {'ok': True, 'op': 'agent.terminate', 'id': '4'}
```

The HMAC covers the request JSON with its fields in the order you sent them. Sign
the exact compact rendering, then append `auth` as the last field.

---

## The native TCP/IP stack (bare metal)

`src/net/inet.cpp` is AIOS's own Ethernet/ARP/IPv4/ICMP/UDP/DHCP/DNS/TCP stack,
including passive open (listen/accept). It runs on the x86 metal kernel over the
virtio-net or e1000 driver, with no host OS underneath. The metal console
exposes it as `host net` (`metal/main.cpp:905`):

```
host net                          # NIC, MAC, fabric counters, whether TCP/IP is up
host net up [ip mask gw dns]      # no args: DHCP
host net status                   # ip/gw/dns + rx/tx/arp/ip/tcp/retransmit/drop counters
host net ping <ip|host>           # ICMP echo, prints µs
host net resolve <host>           # DNS A record
host net get <host> [path] [port] # HTTP/1.0 GET, prints the status line and byte count
host net btc [seed]               # Bitcoin P2P version/verack handshake over this stack
```

The commands only work after `host net up`; otherwise they answer
`net up first, then: host net ping <ip>`. Once the stack is up, every
`net_connect` to an IP or name on that kernel goes through it. That includes
`sense http://…`, the TLS client and `ssh`.

This needs the metal ISO, and `build/metal/` holds only
object files. Building it wasn't allowed in this run. The prepared drive is
`metal/drive-net.py [ISO]`. It boots the image under QEMU user networking and
gates on a DHCP lease (`up: ip 10.0.2.15`) and on `host net ping 10.0.2.2`.

**On a hosted kernel**, `host …` runs the rest of the line as a host command, so
`host net up` runs whatever `net` is on the host's PATH. Driven: on this machine
that's Samba's `net` tool (`net vfs … net help` and then
`FAILED: io-error: host command exited with status 255`). Hosted kernels always
use the host's sockets. Use `network` to list the host interfaces as the kernel
sees them:

```
$ aios exec 'network'
INTERFACE     ADDRESS           HARDWARE            MTU     STATE     NOTE
lo            127.0.0.1         00:00:00:00:00:00   65536   up        loopback
br0           10.60.60.215      52:70:34:95:82:e6   1500    up        1000 Mbit/s
...
```

`p2p status|peers|ping|mdns …` (LAN beacon discovery over UDP broadcast) and
`net check|repair` are separate commands. They broadcast on
the LAN.

---

## Running as a service

Three units ship in `deploy/`. Copy one to `/etc/systemd/system/`, put the binary
at the path it names, run `systemctl daemon-reload`, then
`systemctl enable --now <unit>`. Units were not installed
or started. These are their contents.

### `aios-webserver.service`

`deploy/aios-webserver.service` is the same file as
`packaging/aios-webserver.service.in`:

```
User=aios  Group=aios
ExecStart=/opt/aios/bin/aios-webserver /etc/aios/webserver.conf
AmbientCapabilities=CAP_NET_BIND_SERVICE  CapabilityBoundingSet=CAP_NET_BIND_SERVICE
NoNewPrivileges=true  ProtectSystem=full  ProtectHome=true  PrivateTmp=true  PrivateDevices=true
ProtectKernelTunables=true  ProtectKernelModules=true  ProtectControlGroups=true
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6  RestrictSUIDSGID=true  LockPersonality=true
```

It runs as the unprivileged `aios` user (`install-asql.sh` creates it), keeping
only the right to bind ports below 1024. The `cert`/`key` files a vhost names
must be readable by `aios`; Let's Encrypt's `privkey.pem` is root-only by
default, so give the `aios` group read on a copy (for example from a renewal
deploy hook). `AmbientCapabilities` needs systemd 229 or later; on older
systemd, listen on a port above 1024 (the shipped example uses 18080).

### `aios-gateway.service`

```
After=network.target aios-compute.service
User=aios
Environment=AIOS_NO_ACCEL=1
EnvironmentFile=-/etc/aios/gateway.env
ExecStart=/opt/aios/bin/aios --root /opt/aios/root exec 'gateway serve port=7310 require_custom_secret=1'
NoNewPrivileges=true  ProtectSystem=full  ReadWritePaths=/opt/aios/root
```

Put `AIOS_GATEWAY_SECRET=<long random value>` in `/etc/aios/gateway.env` (mode
600). Without it, `require_custom_secret=1` makes the unit refuse to start. The
unit binds 127.0.0.1:7310; add `public=1` to expose it. To mint tokens for this
gateway, pass `secret=` with the same value (see the gateway section).

### `aios-agent-control.service`

```
User=aios
Environment=AIOS_NO_ACCEL=1
Environment=AIOS_AGENT_CONTROL_MEMORY_MB=256
EnvironmentFile=-/etc/aios/agent-control.env
ExecStart=/opt/aios/bin/aios-agent-control --root /opt/aios/agent-control-root --port 7312 --auth-secret-file /etc/aios/agent-control.key
NoNewPrivileges=true  ProtectSystem=full  ReadWritePaths=/opt/aios/agent-control-root
```

Create `/etc/aios/agent-control.key`, readable by the `aios` user, with one
secret line. The service binds 127.0.0.1:7312 because the unit doesn't pass
`--listen-all`.

### Node, fleet and compute

No unit ships for these. The pattern used on the fleet runs a long-lived
`exec` with a trailing `sleep`, and keeps the key in a file:

```
ExecStart=/opt/aos/bin/aios --root /opt/aos/fleet-root exec 'fleet join <hub> 7420 @/etc/aos/fleet.key' 'sleep 4000000'
```

`node serve`, `fleet hub`, `fleet join` and `fleet observe` accept the secret as
`@FILE` (first line of the file) or `env:NAME` (an environment variable, e.g.
from an `EnvironmentFile=`). A secret passed as a plain argument shows up in
`ps` and `systemctl cat`. `node serve` and `fleet hub` also take
`bind=127.0.0.1` to listen on loopback only.

---
