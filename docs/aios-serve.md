# Hosting Grain applications with AIOS

AIOS hosts Grain applications through its `serve` command. It runs the same application directories and `app.gr` manifests as standalone `grain host`, but adds direct access to ASQL, Things, and the fleet mesh.

Related guides:
- [Running and managing Grain applications](grain-apps.md): deployment lifecycle, secrets, and rollbacks.
- [Writing a Grain application](grain-app-writing.md): manifests, route handlers, and checks.
- [AIOS Fleet Architecture](fleet.md): clusters, hubs, and remote execution.

---

## Running the host daemon

Start the service from the AIOS command line:

```bash
aios --root /opt/aios/apps-root exec "serve port=7480"
```

The daemon keeps its host files in `<root>/apps/` (the same layout as `~/.grain`), so each app's versions, settings, data and log are in `<root>/apps/apps/<name>/` - on chi-il-1, `/opt/aios/apps-root/apps/apps/stickerfeed/`. It binds its control port to `127.0.0.1:7480`:

```text
serve: 1 app(s) in /opt/aios/apps-root/apps; control on 127.0.0.1 port 7480
```

To let remote machines manage apps over the network, add `all-ips=1`:

```bash
aios --root /opt/aios/apps-root exec "serve port=7480 all-ips=1"
```

Stopping with `Ctrl+C` or `SIGTERM` shuts down all running app processes cleanly.

---

## Running as a systemd service

In production (for example on `chi-il-1`), run `serve` as a managed service:

```ini
[Unit]
Description=AIOS Application Host Service
After=network.target

[Service]
Type=simple
User=aios
Group=aios
EnvironmentFile=/etc/aios/apps-fleet.env
Environment=TZ=America/New_York
ExecStart=/opt/aios/bin/aios --root /opt/aios/apps-root exec "serve port=7480"
Restart=always
RestartSec=5
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

Environment file notes:
- `/etc/aios/apps-fleet.env`: Sets `AIOS_FLEET_HUB` and `AIOS_FLEET_KEY` so the host attaches to the cluster hub automatically. Mode `0600`, owned by `aios` or `root`.
- `TZ`: Sets the timezone for date formatting and audit timestamps.

---

## Managing applications

From the AIOS console (or with `aios exec "app ..."`), commands look like `grain app`:

```bash
app deploy apps/stickerfeed         # deploy a local folder
app status                          # show running services
app logs stickerfeed -f             # tail stdout and stderr
app set stickerfeed ASQL_USER=erp   # update setting
cat /opt/aios/mysql.pass | app secret stickerfeed ASQL_PASSWORD  # set secret
app rollback stickerfeed            # revert to previous release
```

By default commands act on the local host. To target a standalone `grain host` running outside AIOS, add `at <host:port> key=@file`.

---

## Fleet orchestration (`on <node>`)

When an AIOS host is joined to a fleet hub, you can deploy and inspect applications across the whole cluster using `on <node>`:

```bash
# Package locally and deploy to a remote fleet member
app deploy apps/stickerfeed on 7c513d4f845a

# Node ID prefixes work too
app logs stickerfeed on 7c51 -f

# Check status across every node in the fleet
app status on ALL

# Roll back on a specific node
app rollback stickerfeed on 7c51
```

The command packages your local application folder into a bundle, streams it over the fleet mesh, and deploys it on the target member.

If the target machine just rebooted, connection attempts will retry for a few seconds before failing.

---

## AIOS builtins in applications

Grain apps running under AIOS have access to extra builtins beyond standard Grain:

- `sql(statement)`: Runs an SQL statement on the local ASQL engine without network roundtrips. Queries return a list of row maps (with nulls as `nil` and numbers parsed); write statements return `{"affected": n}`. Statements that fail raise a Grain error.
- `sql_on(peer_id, statement)`: Dispatches an SQL statement to a peer fleet node over the node link.
- `sys`: Machine details as a map containing `arch`, `host`, `cpus`, `memory`, `kernel`, `uptime_ms`, `entities`, `memory_objects`, and `memory_bytes`.

Declare these dependencies in `app.gr` so the app is never deployed somewhere that can't run it:

```grain
"requires": ["sql", "aios", "fleet"]
```

If you try to deploy this app to a standalone `grain host` without AIOS, the deploy is rejected before copying any files.

---

## Updating ASQL Things during deploys

When a web app serves cached views from an ASQL Thing, you can update the Thing atomically inside `deployed.gr`:

```sql
CREATE OR REPLACE THING sticker_item GRAIN KEY stockid READS ... AS ?
```

ASQL builds the replacement Thing in the background alongside the live one (named `<name>~next`). Any writes that happen during the build are caught up automatically. When the build finishes, ASQL swaps the pointer to the new data with zero downtime.

---

## Routes on another member, and their event streams

`on_machine(member, handler)` gives a route to another fleet member: the request is answered by that member's copy of
the app and the answer comes back here. A request whose answer is a stream - `Accept: text/event-stream`, what a
browser's `EventSource` sends - is carried as a stream: the member's route writes (`writer.event`, `writer.write`)
reach this machine's client as they are written, on a node link of the stream's own (a node `AppStream` request,
`src/node/protocol.hpp`). When the client leaves, the link is closed, the member's next write fails and its route's
`writer.is_closed()` turns true. Every other request is carried as one answer, as before.

A page's own `EventSource` to one of its routes reaches that route; before, a `ui.pane` program served it the pane's
stream of changes instead, whatever the path.

Driven on a private local fleet (hub and two app hosts, 2026-10-27): a route on member A writing an event every
500 ms, asked through member B, delivered them 0.50 s apart, each from A; a client that left after 1.2 s ended A's
route ("reader gone"); a plain route on A came back whole through B.

`apps/warehouse_touches` puts the two together: its `/quickreportevents` route waits on the `touch_day` Thing with
`WAIT THING` (see `docs/asql.md`) and pushes the new figures when a day of the page's range changes.

---

## Things to keep in mind

- **Compute vs Serve**: ASQL compute tasks, binlog replication, and MySQL wire protocol services run under `compute serve`, while Grain application hosting is managed by `serve`.
- **Portability**: Standard `db_connect()` and `db_query()` work the same inside AIOS and in standalone Grain, making services easy to test locally before deploying to the fleet.
