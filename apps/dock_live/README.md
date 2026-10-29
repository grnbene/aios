# Dock Live

The loading dock as it is right now: which truck is on each door, every line it is waiting on, who has each line
claimed and who picked it, and what is on the truck. It feeds the wall board here and the v8 dock screens (the
loading TV `warehouse-checking-wifi` and the scanners' Item Pick `order-pick`), pushed the moment something
changes instead of each screen polling or waiting on a socket relay.

Related: [Things](../../docs/things.md) · [Running Grain apps](../../docs/grain-apps.md) · [warehouse_touches](../warehouse_touches/README.md) (same shape)

---

## How it works

1. **One Thing, `dock_bay`, kept by ASQL from Cleve's tables** (by CDC): one row per door (`loadingbays.bay_id`) -
   the truck on it, its driver (from the run's `dispatch.driveruserid`), its orders in load order, each line's
   quantity / picked / loaded / bin / picker / claim and their times, and the door's totals. A change rebuilds only
   the door its order's truck is on, and only when a column the board uses changed (`USED` in `lib/dock_sql.gr`).

   **Why there is no per-pick-row Thing.** `dock_pick` (one row per open pick) was built and dropped on 2026-10-29:
   it reacted to every pick-row change in the warehouse, ~450 a minute, and every reaction holds the engine's single
   Things lock (~300 ms each on atl whatever the program does). `SHOW THING` for every Thing on atl - this board,
   the touches report - then stalled 4-16 s at a time; with it gone the worst read is ~17 ms. Keep a Thing's keys to
   what changes a few times a minute.
2. **The app reads the Thing and never writes.** Every claim, pick and load is still made by the v8 PHP API in
   Cleve's MariaDB. It reaches ASQL by CDC and comes back out of here to every screen at once.
3. **Pushed, not polled.** `/stream` sits in `WAIT THING`, which the engine wakes when it commits a change, and sends
   the changed rows as server-sent events. The wall board is a `ui.pane` with `watch` on `dock_bay`.

Measured on a local ASQL with Cleve's data (2026-10-29): a claim written to `pickingfreeze` reached a `WAIT THING`
in 52 ms and a browser on `/stream` in 39-41 ms; a truck assigned to a door, 49 ms, with that door and all 15 of the
truck's pick rows in the one change.

## Addresses

| Address | Gives |
|---|---|
| `/` | The wall board. `?bays=1,2,3` (the default) picks the doors; `?theme=light` or `dark` sets the screen's theme (kept, as is a pick made with the board's Light/Dark button; light otherwise) |
| `/bays` | `{success, clock, bays: [...]}`; `?bays=1,3` for some |
| `/stream?kind=bays` | Server-sent events: `snapshot` (every row) on connect, `change` `{clock, rows, gone}` as they land, `ping` every 20 s |
| `/dockauth` | The key check atl's nginx asks before every request (204 / 401) |
| `/health` | `{"status":"ok"}` |

On atl these are reached as `https://atl-ga-1.tireserv.com/?route=apir/warehouse/dock/<board|bays|stream>&key=<api key>&userid=<user>`
(see `deploy/atl-nginx.conf`). The key and userid are what the v8 client already signs every PHP call with.

## The v8 screens

The Cleve client config names it: `apiServer.dockLive` = `https://atl-ga-1.tireserv.com/?route=apir/warehouse/dock/`
(Cleve's login "Vans" reads `config.prod.json`). A browser keeps the config it got at company pick for good, so
`DockLiveService` (`client/src/app/services/dock-live.service.ts`) reads the address from the tenant's file when the
kept config lacks it. One stream for the whole app.

- **Loading screen (`warehouse/warehouse-loading`):** a "Live board" toggle, on by default, frames this board in place
  of the classic screen; the classic screen keeps running underneath (load scans, error sounds, completing a bay)
  and comes back while a bay needs the loader. atl's nginx lets the Cleve v8 hosts frame this route only.
- **Loading TV (`warehouse-loading-wifi`):** claims, picker, counts and bin are written onto the lines on screen. A
  different truck on a door, or a different set of orders on it, rereads that door from PHP.
- **Item Pick:** the bay filter's order map comes from `bays`, so a truck put on a door shows on the scanner at once;
  claims made elsewhere reread its list from PHP.

Without `dockLive` in the config, or while a stream is down (atl unreachable, nginx not set up, a key refused),
both screens run exactly as before: their own sockets, the bay map from PHP every 60 s, the TV's 30 s reread.

## Deploying

```bash
app set dock_live DB_USER=erp
cat /opt/aios/mysql.pass | app secret dock_live DB_PASSWORD
app deploy apps/dock_live on atl-ga-1
```

`deployed.gr` creates `dock_bay` (or replaces it in place) and drops `dock_pick` where an earlier version made it. Then add `deploy/atl-nginx.conf` to
atl's nginx, reload it, and deploy the v8 client with the config entry above. Order matters only for noise: a client
pointed at atl before nginx answers keeps retrying every 30 s and otherwise behaves as before.

## Files

```text
apps/dock_live/
├── app.gr               # name, port (8577), settings and secrets
├── service.gr           # /bays, /stream, /dockauth, the board
├── deployed.gr          # creates or replaces dock_bay
├── checks.gr            # pre-cutover: health, board, key check refuses no key
├── things/
│   └── dock_bay.gr      # one row per door
├── lib/
│   ├── dock_sql.gr      # what both Things read (inside the engine)
│   └── views.gr         # the wall board
└── deploy/
    └── atl-nginx.conf   # the atl routes
```
