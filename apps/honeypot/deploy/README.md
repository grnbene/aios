# Deploying the honeypot fleet

One **hub** (runs the dashboard and collects everything) and any number of **nodes** (your real servers,
trapping probes). The hub is also a node. Sample configs: [`hub.conf`](hub.conf), [`node.conf`](node.conf).

## 0. Build (once, then copy the binaries)

```sh
cmake -B build && cmake --build build --target aios aios-webserver -j
# copy build/aios and build/aios-webserver to /usr/local/bin on every box
```

## 1. Fleet secret (once, then the same file goes on every box)

```sh
sudo mkdir -p /etc/aios && openssl rand -hex 24 | sudo tee /etc/aios/fleet.token >/dev/null
sudo chmod 600 /etc/aios/fleet.token
```

Without this secret, the hub rejects every event from other machines with a 401. Don't put the secret
in a config file you share. It never appears in logs.

## 2. Hub

```sh
# the dashboard app
aios --root /var/lib/aios exec "serve port=7480" &          # the app host (keep it running: systemd below)
aios --root /var/lib/aios exec "app deploy /path/to/aios/apps/honeypot"
aios --root /var/lib/aios exec "app set honeypot FLEET_TOKEN=$(sudo cat /etc/aios/fleet.token) DASH_PASSWORD=<pick one> AIOS_NODE=hub"
aios --root /var/lib/aios exec "app restart honeypot"       # REQUIRED: settings only apply after a restart

# the hub's own trap (forwards to its own dashboard on 127.0.0.1)
aios-webserver /etc/aios/hub.conf
```

Dashboard: `http://<hub>:8577/`. Log in with any username and `DASH_PASSWORD`.

- If `DASH_PASSWORD` isn't set, the dashboard only answers the hub box itself.
- If `FLEET_TOKEN` isn't set, `/ingest` only accepts events from the hub box itself.

## 3. Each node

Copy `/etc/aios/fleet.token` from the hub. Then add the `env_honeypot_*` lines from `node.conf` to that
server's existing vhost (or use `node.conf` as a starting point). Set `AIOS_NODE` so the node has a
readable name on the dashboard:

```sh
AIOS_NODE=sstore aios-webserver /etc/aios/node.conf
```

Within a minute the node appears in the dashboard's **Fleet** panel as *reporting*. If it shows **NOT
REPORTING**, or never appears, check these:

- the hub URL in `env_honeypot_forward` is reachable from the node: `curl https://hub.example.com/health`
- the token file matches the hub's `FLEET_TOKEN`. On a mismatch, the node's log says `hub ... refused this node (HTTP 401)`.
- port 8577 is open on the hub's firewall to your nodes (and only to your nodes)

## 4. Network: read this one

The dashboard and `/ingest` speak **plain HTTP** on port 8577. That means the dashboard password and the
fleet token cross the wire unencrypted unless you do one of these:

- **Private network (simplest):** put the hub and nodes on WireGuard or Tailscale, and point
  `env_honeypot_forward` at the hub's private address. Keep 8577 closed to the public internet.
- **TLS in front:** put Caddy or nginx in front of 8577 with a certificate, and use
  `https://hub.example.com/ingest` in `env_honeypot_forward` (nodes forward over HTTPS natively).

Don't expose 8577 to the internet with neither in place.

## 5. Optional but worth it

- **Callbacks:** point a subdomain you own (for example `cdn-assets.example.com`) at a node, and set
  `env_honeypot_collect_url` to it. That's how an attacker's tooling testing the stolen decoy creds
  shows up as a red callback. Without it you only see grabs.
- **Geo + map:** `env_honeypot_geo` takes a CSV of `startIPv4int,endIPv4int,LABEL` sorted by start. The
  label can carry `type=hosting|residential|...` and `lat=.. lon=..` tokens. Build it from GeoLite2 (free
  MaxMind account) joined with city coordinates. Without it, the map stays empty. Everything else still
  works, including the **identify** button (RDAP, ASN, reverse DNS, Spamhaus, Tor), which needs nothing local.

## 6. Keep it running (systemd)

```ini
# /etc/systemd/system/aios-honeypot-edge.service   (every box)
[Service]
Environment=AIOS_NODE=sstore
ExecStart=/usr/local/bin/aios-webserver /etc/aios/node.conf
Restart=always
[Install]
WantedBy=multi-user.target

# /etc/systemd/system/aios-apphost.service          (hub only)
[Service]
ExecStart=/usr/local/bin/aios --root /var/lib/aios exec "serve port=7480"
Restart=always
[Install]
WantedBy=multi-user.target
```

`systemctl enable --now aios-honeypot-edge aios-apphost`

## Reporting

The dashboard's **incident report** and **export JSON/CSV** buttons produce the files to hand over. In the
US, file at https://www.ic3.gov and attach both. For a specific origin, **identify** gives the network
owner's abuse contact. Their abuse desk can act on their own customer faster than anyone else can.
