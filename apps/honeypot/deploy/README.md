# Deploying the honeypot fleet

Two commands total: one on your hub, then one on each server.

## 1. The hub (the server running your aios-webserver)

From a checkout of this repo on that server:

```sh
sudo apps/honeypot/deploy/install-hub.sh
```

It finds your running aios-webserver and its config, then:

- builds what's missing, including a static `aios-webserver` that runs on any x86_64 Linux (your servers download it from the hub)
- creates the fleet secret and a dashboard password, both 0600 in `/etc/aios`
- runs the dashboard as the `aios-apphost` systemd service, reachable on 127.0.0.1 only
- adds `honeypot_hub /honeypot` (plus a trap for the hub itself) to your vhost. The file is backed up and checked
  with `aios-webserver --check` first; if the check fails, the original is restored and nothing changes.
- restarts your aios-webserver, if it runs under systemd. Otherwise it tells you to restart it.
- prints the dashboard address, the password, and the join command

Re-running it is safe, and it's also how you upgrade: it refreshes its own lines and the binaries.

Options:

| Option | What it does |
|---|---|
| `--config FILE` | if aios-webserver isn't running yet |
| `--vhost NAME` | which vhost to use (default: the first one with ssl) |
| `--url https://host/honeypot` | if the vhost name isn't your public domain |
| `--password PW` | set the dashboard password yourself |
| `--no-trap` | don't add a trap for the hub itself |
| `--no-restart` | don't restart your aios-webserver |
| `--uninstall` | remove what it added; recorded evidence stays |

## 2. Each server running nginx or Apache

Open the dashboard (`https://<your hub>/honeypot/`). Under **Fleet → Add a server**, copy the command and run it
on the server, replacing `NAME`:

```sh
curl -fsSL -H "X-Fleet-Token: …" https://<your hub>/honeypot/join.sh | sudo bash -s -- --name NAME
```

In about a second, it:

- detects nginx or Apache
- downloads the sidecar from your hub
- starts the sidecar as the `aios-honeypot` service on 127.0.0.1
- wires every site, with backups, an `nginx -t` / `apachectl configtest` check, and automatic restore on failure
- sends itself a fake `.env` request to prove the trap works

The server then shows green on the dashboard. Your sites keep serving normally. Only bait requests go to the
honeypot: `.env*`, `wp-config.php`, `.git/config`, `../` traversal, and `/px/` callbacks.

| Command suffix | What it does |
|---|---|
| `--uninstall` | removes every line it added (files come back byte-identical), stops the sidecar, and drops the server from the dashboard |
| `--collect-url https://sub.yourdomain/px` | turns on bait callbacks (see below) |
| `--front nginx` or `--front apache` | if both run on the server |

If something fails, the script says exactly what (for example, nginx's own error, including file and line) and leaves the server
exactly as it was.

## Worth knowing

- **Behind Cloudflare?** Turn on nginx's `realip` module or Apache's `mod_remoteip` with Cloudflare's IP ranges.
  Otherwise every attacker shows up as a Cloudflare address.
- **Bait callbacks:** point a subdomain you own at one of your servers and join it with
  `--collect-url https://that.subdomain/px`. When attacker tooling tests the stolen decoy credentials, it shows
  up as a red callback.
- **Map:** put a geo-IP CSV at `/etc/aios/geo.csv` before joining. The format is `startIPv4int,endIPv4int,LABEL`
  with `type=` and `lat=`/`lon=` tokens. Without it the map is empty. Everything else works without it,
  including **identify** (RDAP, ASN, reverse DNS, Spamhaus, Tor).
- **A server running aios-webserver directly** (no nginx or Apache): add the `env_honeypot_*` lines from
  [`node.conf`](node.conf) to its vhost.
- **Not x86_64** (for example, ARM): build `aios-webserver` on that server and use [`sidecar.conf`](sidecar.conf)
  with [`nginx-honeypot.conf`](nginx-honeypot.conf) or [`apache-honeypot.conf`](apache-honeypot.conf) by hand.
  Those files show what `join.sh` does automatically.

## Reporting

The dashboard's **incident report** and **export JSON/CSV** buttons produce the files to hand over. In the US,
file at https://www.ic3.gov and attach both. For one origin, **identify** gives the network owner's abuse
contact. Their abuse desk can act on their own customer faster than anyone else can.
