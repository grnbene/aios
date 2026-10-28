# Warehouse Touches

A report of who touched what in the warehouse, and how much: receiving, picking, bin moves, truck loading, sticker
scans and customer pickups, per person, over a range of days. It replaces the PHP report `touches.php` and gives the
same numbers.

Related: [Running Grain apps](../../docs/grain-apps.md) · [Writing Grain apps](../../docs/grain-app-writing.md) ·
[Things](../../docs/things.md)

---

## How it works

The heavy work is done once, by ASQL, not on every page view.

1. **ASQL keeps a Thing called `touch_hour`.** It has one row per warehouse hour (`"2026-10-27 08"`), holding
   each person's figures for that hour. ASQL builds it from the warehouse tables and, when a row in one of those
   tables changes, it recomputes only the hour that row falls in. The program for it is `things/touch_hour.gr`.
2. **The app reads that Thing.** A report for a range reads the hours in the range and adds them up per person
   (`lib/queries.gr`). It does not query the warehouse tables itself.
3. **Deploying the app creates the Thing.** After a deploy, `deployed.gr` creates `touch_hour` in the ASQL the app
   points at, or replaces it with the new program if it already exists. While the new version is being built, the
   old one keeps answering.

The tables `touch_hour` reads: `recscan`, `pickingfreeze`, `stockmaster`, `product_types`, `binmovements_details`,
`www_users`, `loadingbays`, `dispatch`, `stickerscanout`, `salesorderdetails`, `trackinginfo`, `salesorders`,
`debtortrans`.

## Where it runs

| Machine | What it does |
|---|---|
| **atl-ga-1** | Does the work. Its ASQL keeps `touch_hour`, and its copy of the app answers every request. The public page goes through atl's nginx. |
| **Other machines** (e.g. a workstation) | Can run a copy that forwards everything to atl. With `WAREHOUSE_COMPUTE_ON` set to atl's app host id, the page and every report and export are answered by atl and sent back. Nothing is computed locally, and the deploy there does not create a Thing. |

`WAREHOUSE_COMPUTE_ON` empty (the default) means "answer here, from this machine's ASQL".

## The page

`/` is the report page. It shows summary cards, the mix of touch types, a table of operators you can sort, and a
timeline of the day (Hours, Days, Heatmap tabs). Pick dates at the top; click an hour or drag across hours to narrow
the report to them.

The page updates by itself: when ASQL changes `touch_hour`, every open page checks whether its own range changed and
redraws if it did.

## Addresses

| Address | Gives |
|---|---|
| `/` | The report page |
| `/quickreportview` | The old PHP address. Sends you to `/` with the same dates and hours, so old links keep working |
| `/quickreport` | The report as JSON |
| `/quickreportpdf` | The report as a PDF (landscape letter) |
| `/quickreportxlsx` | The report as an Excel file |
| `/quickreportcsv` | The report as CSV |
| `/quickreportauth` | Used by atl's nginx to check the API key before letting a request to the public page through |
| `/health` | `{"status":"ok"}` |

### Choosing dates and hours

All addresses above take the same options in the link:

| Option | Example | Meaning |
|---|---|---|
| `start` | `2026-10-01` | First day. Default: 29 days before `stop` (30 days in all) |
| `stop` | `2026-10-27` | Last day. Default: today |
| `range` | `2026-10-01..2026-10-27` | Both days at once |
| `hours` | `7-11` | Only these hours of each day (7 through 11) |
| `hour_wanted` | `10` | One hour of each day, as the PHP report took it |

A date that isn't a real date is ignored and the default is used.

`userid` is **not** a filter. Reports always cover everyone. On the public page, `userid` is only part of the login
check (see below).

## The public page

atl serves the page to outside users at:

```
https://atl-ga-1.tireserv.com/?route=apir/warehouse/touches/quickreportlive&key=<api key>&userid=<user>
```

Add `&export=pdf`, `&export=xlsx` or `&export=csv` for a file. Before each request nginx asks the app
(`/quickreportauth`) whether to allow it, using the same rule as the PHP API: the key must be an active key in
`apikeys` for this database, and `userid` must be set and not `admin`. Otherwise the answer is 401.

---

## Files

```text
apps/warehouse_touches/
├── app.gr               # name, port (8573), settings and secrets
├── service.gr           # the page, the addresses, the login check
├── deployed.gr          # runs after a deploy: creates or replaces the touch_hour Thing
├── things/
│   └── touch_hour.gr    # the Thing: one row per hour, each person's figures
├── lib/
│   ├── queries.gr       # reads touch_hour for a range and adds it up per person
│   └── views.gr         # the page, PDF, Excel and CSV
└── README.md
```

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `DB_HOST` | `127.0.0.1` | ASQL address |
| `DB_PORT` | `3307` | ASQL port |
| `DB_USER` | `root` | Database user (atl uses `erp`) |
| `DB_NAME` | `versioneight` | Database |
| `WAREHOUSE_COMPUTE_ON` | empty | Fleet member that answers instead of this machine (see "Where it runs") |

The password is either the secret `DB_PASSWORD` or a file named by the setting `DB_PASSWORD_FILE` (atl uses
`/opt/aios/mysql.pass`).

```bash
grain app set warehouse_touches DB_USER=erp DB_PASSWORD_FILE=/opt/aios/mysql.pass
# or keep the password as a sealed secret (read from stdin, never typed on the command line):
grain app secret warehouse_touches DB_PASSWORD < /path/to/password-file

# a copy on another machine that sends everything to atl:
grain app set warehouse_touches WAREHOUSE_COMPUTE_ON=<atl app host id>
```

Add `at <host>` to any of these to change another machine's settings.

## Running it

Deploy it to an app host (this also creates the Thing):

```bash
grain app deploy apps/warehouse_touches at <host>
grain app status warehouse_touches at <host>
grain app logs warehouse_touches at <host>
```

On its own, for trying it out against an ASQL that already has `touch_hour`:

```bash
grain run apps/warehouse_touches/service.gr
```
