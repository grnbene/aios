# Deploying the honeypot fleet

```
 attacker ──> nginx / Apache (your site, unchanged)
                 │  only bait requests: .env, wp-config.php, .git/config, ../ traversal, /px/ callbacks
                 ▼
              aios-webserver SIDECAR on 127.0.0.1:8899   (decoy, trace, substrate record, heartbeat)
                 │  HTTPS + fleet token
                 ▼
 https://your-aios-server/honeypot/ingest   (aios-webserver: honeypot_hub, your TLS)
                 │
                 ▼
              dashboard app on 127.0.0.1:8577   ->   https://your-aios-server/honeypot/
```

Files here:

| File | What it's for |
|---|---|
| [`hub.conf`](hub.conf) | your aios-webserver hub |
| [`sidecar.conf`](sidecar.conf) | the sidecar next to nginx or Apache |
| [`nginx-honeypot.conf`](nginx-honeypot.conf) | the nginx snippet |
| [`apache-honeypot.conf`](apache-honeypot.conf) | the Apache snippet |
| [`node.conf`](node.conf) | a server that runs aios-webserver as its main web server |

## 0. Build once, then copy the binaries

```sh
cmake -B build && cmake --build build --target aios aios-webserver -j
# copy build/aios and build/aios-webserver to /usr/local/bin on every box
```

## 1. Fleet secret: create it once, then put the same file on every box

```sh
sudo mkdir -p /etc/aios && openssl rand -hex 24 | sudo tee /etc/aios/fleet.token >/dev/null
sudo chmod 600 /etc/aios/fleet.token
```

The hub rejects any event that doesn't carry this secret. The secret never appears in any log.

## 2. The hub (your aios-webserver)

```sh
# the dashboard app, on 127.0.0.1 only
aios --root /var/lib/aios exec "serve port=7480" &           # app host (systemd unit below)
aios --root /var/lib/aios exec "app deploy /path/to/aios/apps/honeypot"
aios --root /var/lib/aios exec "app set honeypot FLEET_TOKEN=$(sudo cat /etc/aios/fleet.token) DASH_PASSWORD=<pick one> AIOS_NODE=hub"
aios --root /var/lib/aios exec "app restart honeypot"        # REQUIRED: settings only apply after a restart
```

Add the `honeypot_hub` line to your existing aios-webserver vhost (see `hub.conf`):

```
honeypot_hub /honeypot http://127.0.0.1:8577
```

- **Dashboard:** `https://your-aios-server/honeypot/`. Log in with any username and `DASH_PASSWORD`.
- **Nodes report to:** `https://your-aios-server/honeypot/ingest`

The app's own port (8577) stays on 127.0.0.1. Keep it closed in your firewall; the relay is the only way in,
over your TLS.

## 3a. A node running nginx or Apache: the sidecar

```sh
sudo mkdir -p /var/lib/aios/sidecar-empty /var/log/aios
sudo cp sidecar.conf /etc/aios/sidecar.conf      # edit the CHANGE-ME lines: your hub URL, collect_url
AIOS_NODE=sstore aios-webserver /etc/aios/sidecar.conf     # name it; that's what the dashboard shows
```

**nginx:**

```sh
sudo cp nginx-honeypot.conf /etc/nginx/snippets/aios-honeypot.conf
# inside each  server { ... }  you want trapped:
#     include snippets/aios-honeypot.conf;
sudo nginx -t && sudo systemctl reload nginx
```

**Apache 2.4.10+:**

```sh
sudo a2enmod proxy proxy_http rewrite headers
sudo cp apache-honeypot.conf /etc/apache2/conf-available/aios-honeypot.conf
# inside each  <VirtualHost ...>  you want trapped:
#     Include conf-available/aios-honeypot.conf
sudo apachectl configtest && sudo systemctl reload apache2
```

Your site keeps serving exactly as before. Only bait requests go to the sidecar.

Attribution: the front server writes the real client address into `X-Real-IP`, and the sidecar believes that
header only because the request comes from 127.0.0.1. A `CF-Connecting-IP`, `X-Forwarded-For` or `X-Real-IP`
that the attacker sends to frame someone else is kept in the record as evidence but never used as the origin.

**Behind Cloudflare?** Set up nginx's `realip` module or Apache's `mod_remoteip` with Cloudflare's IP ranges,
so the front server's own client address is the real visitor and not Cloudflare.

## 3b. A node running aios-webserver itself

Add the `env_honeypot_*` lines from `node.conf` to its vhost, and start it with `AIOS_NODE=<name>`.

## 4. Check it

Within a minute, each node appears in the dashboard's **Fleet** panel as green *reporting*. If one doesn't:

- From the node: `curl https://your-aios-server/honeypot/health` should print `{"ok":true}`.
- The node's log says `hub ... refused this node (HTTP 401)`: its `fleet.token` doesn't match the hub's
  `FLEET_TOKEN`.
- The node's log says `could not deliver to hub ... after 3 tries`: there's a network or URL problem.
- To trap yourself on purpose: `curl https://<node>/.env`. It should show up on the dashboard with your
  own IP.

## 5. Optional but worth it

- **Callbacks:** point a subdomain you own (for example `cdn-assets.example.com`) at a node, and set it as
  `env_honeypot_collect_url`. That's how an attacker's tooling testing the stolen decoy credentials shows up
  as a red callback. The nginx and Apache snippets already route `/px/` to the sidecar.
- **Geo and map:** `env_honeypot_geo` takes a CSV of `startIPv4int,endIPv4int,LABEL`, sorted by start, where
  the label can carry `type=` and `lat=` / `lon=` tokens (for example, built from GeoLite2). Without it the
  map stays empty. **Identify** (RDAP, ASN, reverse DNS, Spamhaus, Tor) works without it.

## 6. Keep it running (systemd)

```ini
# /etc/systemd/system/aios-honeypot-sidecar.service    (each nginx/Apache node)
[Service]
Environment=AIOS_NODE=sstore
ExecStart=/usr/local/bin/aios-webserver /etc/aios/sidecar.conf
Restart=always
[Install]
WantedBy=multi-user.target

# /etc/systemd/system/aios-apphost.service             (hub)
[Service]
ExecStart=/usr/local/bin/aios --root /var/lib/aios exec "serve port=7480"
Restart=always
[Install]
WantedBy=multi-user.target
```

`systemctl enable --now aios-honeypot-sidecar` (on nodes) / `aios-apphost` (on the hub)

## Reporting

The dashboard's **incident report** and **export JSON/CSV** buttons produce the files to hand over. In the US,
file at https://www.ic3.gov and attach both. For a specific origin, **identify** gives the network owner's
abuse contact. Their abuse desk can act on their own customer faster than anyone else can.
