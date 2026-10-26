# Stickerfeed service

Stickerfeed is a Grain web service that serves vendor catalog feeds, real-time stock levels, and brand authorization checks for vendor ERP systems.

It replaces the legacy PHP script (`stickerfeedapi.php`) with the exact same API contract, but queries ASQL and a precomputed ASQL Thing (`sticker_item`) so full 11 MB catalog feeds return in ~280 ms.

Related guides:
- [Running and managing Grain applications](../../docs/grain-apps.md)
- [Writing Grain applications](../../docs/grain-app-writing.md)
- [Hosting applications with AIOS](../../docs/aios-serve.md)

---

## API endpoints

All data endpoints require an `Authorization: Bearer <token>` header issued to the vendor. Unauthenticated or invalid requests get HTTP 401.

| Endpoint | Method | Request | Response |
|---|---|---|---|
| `/feed` | `POST` | `{}` or `{"brands": ["NAME", ...]}` | `{"items": [...]}` — full catalog or filtered by brand |
| `/getstock` | `POST` | `{"sku": "X"}` or `{"skus": ["A", "B"]}` | `{"items": [...], "not_found": [...]}` — stock for requested SKUs |
| `/getbrands` | `POST` | none | `{"brands": [...]}` — list of brands the vendor can order |
| `/` | `GET` | none | Operations status dashboard |

---

## Files in this app

```text
apps/stickerfeed/
├── app.gr                 # Manifest (port 8571, local binding, secrets)
├── service.gr             # Route handlers
├── lib/
│   └── feed.gr            # Pricing, markup, and discount rules
├── things/
│   └── sticker_item.gr    # Definition for the sticker_item ASQL Thing
├── checks.gr              # Pre-cutover tests (verifies status and 401 checks)
└── deployed.gr            # Post-deploy hook (swaps sticker_item in ASQL)
```

Both `service.gr` and `things/sticker_item.gr` import `lib/feed.gr`, so pricing and stock math are defined in one place.

---

## Configuring a host machine

Run once per host to configure database connections and inject credentials:

```bash
# Environment settings
app set stickerfeed \
    ASQL_USER=erp \
    STICKERFEED_WRITE_HOST=127.0.0.1 \
    STICKERFEED_WRITE_USER=api \
    STICKERFEED_WRITE_DB=versioneight \
    STICKERFEED_KEY_FILE=/var/www/keyfile.txt

# Database and signing secrets (passed via stdin)
cat /opt/aios/mysql.pass | app secret stickerfeed ASQL_PASSWORD
cat /opt/aios/stickerfeed/write.pass | app secret stickerfeed STICKERFEED_WRITE_PASSWORD
```

*(On standalone Grain hosts outside AIOS, use `grain app` instead of `app`.)*

### Settings reference

| Setting | Default | Description |
|---|---|---|
| `ASQL_HOST` | `127.0.0.1` | ASQL database host |
| `ASQL_PORT` | `3307` | ASQL port |
| `ASQL_USER` | `root` | Database user for reading catalog data |
| `ASQL_DB` | `versioneight` | Database name |
| `STICKERFEED_WRITE_HOST` | *(none)* | MariaDB host for connection audit logs |
| `STICKERFEED_WRITE_PORT` | `3306` | MariaDB port for audit logs |
| `STICKERFEED_WRITE_USER` | *(none)* | User for writing audit logs |
| `STICKERFEED_WRITE_DB` | `versioneight` | Database for audit logs |
| `STICKERFEED_KEY_FILE` | `/var/www/keyfile.txt` | Key file used to verify vendor tokens |

### Secrets reference

| Secret | Description |
|---|---|
| `ASQL_PASSWORD` | Password for reading ASQL |
| `STICKERFEED_WRITE_PASSWORD` | Password for writing MariaDB audit logs |
| `STICKERFEED_ENCRYPTION_KEY` | Direct signing key (if not using `STICKERFEED_KEY_FILE`) |

---

## Deploying

Deploy to a fleet node:

```bash
app deploy apps/stickerfeed on <node_id>
```

Or to a standalone host:

```bash
grain app deploy apps/stickerfeed at <host:port>
```

What happens:
1. The new release starts on a temporary staging port.
2. The host checks the health endpoint and runs `checks.gr` (making sure `/` responds and unauthorized requests return 401).
3. The host swaps the port to `8571` without dropping in-flight requests.
4. `deployed.gr` kicks off a background rebuild of the `sticker_item` ASQL Thing. The existing Thing serves queries until the rebuild is complete.

---

## Monitoring and operational commands

```bash
# Status and uptime
app status stickerfeed on <node>

# Follow logs
app logs stickerfeed -f on <node>

# Roll back to previous version
app rollback stickerfeed on <node>

# Restart process
app restart stickerfeed on <node>
```

---

## Troubleshooting

| What you see | Why it happens | What to do |
|---|---|---|
| `401` with `Missing Authorization: Bearer header` | The request had no `Authorization: Bearer <token>` header | Send the vendor's token in that header. |
| `401` with `Invalid or expired token` | The token isn't one of the vendor's stored tokens, or it has expired | Check the vendor's row in `sticker_feed_exclusions`. If every token fails, the key the tokens were sealed with may not match this machine: check `STICKERFEED_KEY_FILE` (or `STICKERFEED_ENCRYPTION_KEY`). |
| `Missing sticker feed encryption key` in the log | Neither the key file nor `STICKERFEED_ENCRYPTION_KEY` exists on this machine | Put the key file in place, or `app secret stickerfeed STICKERFEED_ENCRYPTION_KEY`, then `app restart stickerfeed`. |
| Deploy stops with `cannot reach ASQL: ...` | The post-deploy step can't connect | Check `ASQL_HOST`, `ASQL_PORT`, `ASQL_USER` (`app settings stickerfeed`) and the `ASQL_PASSWORD` secret (`app secrets stickerfeed` lists what is set). |
| Slow `/feed` right after first deploy | Thing is still building initial data | Check progress in ASQL with `SHOW THING sticker_item`. Subsequent queries will be instant. |
| Timezone skew on `lastsent` timestamps | Host system timezone mismatch | Set `Environment=TZ=...` in the systemd service file and restart the host daemon. |

---

## Notes on the legacy PHP implementation

The legacy PHP feed wrote to `stockmaster.actualcost` during catalog reads. ASQL separates reads from writes, so catalog reads here are strictly read-only. This eliminates write lock contention and guarantees predictable query speeds.
