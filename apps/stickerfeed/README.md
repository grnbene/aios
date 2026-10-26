# Stickerfeed Service

Stickerfeed is a high-performance web service built with Grain and ASQL. It exposes vendor catalog feeds, real-time inventory checks, and brand authorization endpoints for external ERP systems.

It replaces the legacy PHP implementation (`stickerfeedapi.php`) while maintaining 100% byte-for-byte JSON payload compatibility. By migrating reads to ASQL and leveraging precomputed rows (`sticker_item`), large catalog feeds (~11 MB) return in ~280 ms without incurring write locks on the primary database.

Related documentation:
- [Running and managing Grain applications](../../docs/grain-apps.md)
- [Writing Grain applications](../../docs/grain-app-writing.md)
- [Hosting applications with AIOS](../../docs/aios-serve.md)

---

## API Endpoints

All endpoints require an `Authorization: Bearer <token>` HTTP header assigned to the vendor. Unauthenticated or invalid requests receive HTTP 401.

| Endpoint | Method | Request Payload | Response | Description |
|---|---|---|---|---|
| `/feed` | `POST` | `{}` or `{"brands": ["NAME", ...]}` | `{"items": [...]}` | Full catalog feed or filtered by brand name |
| `/getstock` | `POST` | `{"sku": "X"}`, `{"skus": ["A", "B"]}`, or `{"lines": [{"sku": ...}]}` | `{"items": [...], "not_found": [...]}` | Real-time inventory and pricing for specific SKUs |
| `/getbrands` | `POST` | *(none)* | `{"brands": [...]}` | List of authorized brands available to the vendor |
| `/` | `GET` | *(none)* | Operations dashboard | Internal health and call status UI |

---

## Architecture and File Layout

```text
apps/stickerfeed/
├── app.gr                 # Application manifest (port 8571, listen interface, secrets)
├── service.gr             # HTTP routes, authentication, logging, and status UI
├── lib/
│   └── feed.gr            # Pricing, markup, discount resolution, and formatting
├── things/
│   └── sticker_item.gr    # Definition for the reactive `sticker_item` ASQL Thing
├── checks.gr              # Pre-deployment verification (status and 401 responses)
└── deployed.gr            # Post-deployment hook (initializes or hot-swaps `sticker_item`)
```

- **Shared Arithmetic**: Both `service.gr` and `things/sticker_item.gr` import `lib/feed.gr`, guaranteeing that pricing, discounts, and inventory availability calculations are identical whether queried dynamically or served from precomputed state.
- **Dual-Path Resolution**: The `/feed` and `/getstock` endpoints attempt to read precomputed rows from `sticker_item`. If the Thing is still building or the vendor's price level is not pre-indexed, `service.gr` falls back to an on-demand multicore parallel query across ASQL.

---

## Host Configuration

Run once per host machine to configure database connections and inject credentials:

```bash
# Environment settings
app set stickerfeed \
    ASQL_USER=erp \
    STICKERFEED_WRITE_HOST=127.0.0.1 \
    STICKERFEED_WRITE_USER=api \
    STICKERFEED_WRITE_DB=versioneight \
    STICKERFEED_KEY_FILE=/var/www/keyfile.txt

# Database passwords and signing keys (passed via stdin)
cat /opt/aios/mysql.pass | app secret stickerfeed ASQL_PASSWORD
cat /opt/aios/stickerfeed/write.pass | app secret stickerfeed STICKERFEED_WRITE_PASSWORD
```

*(On standalone Grain hosts outside AIOS, substitute `grain app` for `app`.)*

### Settings Reference

| Setting | Default | Description |
|---|---|---|
| `ASQL_HOST` | `127.0.0.1` | ASQL database host (catalog reads) |
| `ASQL_PORT` | `3307` | ASQL port |
| `ASQL_USER` | `root` | Database user for reading catalog data |
| `ASQL_DB` | `versioneight` | ASQL catalog database name |
| `STICKERFEED_WRITE_HOST` | *(none)* | MariaDB host for audit connection logs |
| `STICKERFEED_WRITE_PORT` | `3306` | MariaDB port for audit logs |
| `STICKERFEED_WRITE_USER` | *(none)* | MariaDB user for writing audit logs |
| `STICKERFEED_WRITE_DB` | `versioneight` | MariaDB database for audit logs |
| `STICKERFEED_LOG_TABLE` | `sticker_feed_connection_log` | Table name for connection audit logs |
| `STICKERFEED_LOG_SUCCESS` | `0` | Set to `1` to log successful requests (default logs only failures) |
| `STICKERFEED_KEY_FILE` | `/var/www/keyfile.txt` | Key file used to decrypt vendor tokens |

### Secrets Reference

| Secret | Description |
|---|---|
| `ASQL_PASSWORD` | Password for ASQL catalog reads |
| `STICKERFEED_WRITE_PASSWORD` | Password for MariaDB audit logging |
| `STICKERFEED_ENCRYPTION_KEY` | Hex or raw signing key (fallback if `STICKERFEED_KEY_FILE` is unused) |

---

## Deployment Workflow

Deploy to a fleet node:

```bash
app deploy apps/stickerfeed on <node_id>
```

Or deploy to a standalone host:

```bash
grain app deploy apps/stickerfeed at <host:port>
```

**Lifecycle execution:**
1. A new release starts on an isolated staging port.
2. The orchestrator runs health probes and executes `checks.gr` (verifying `/` answers HTTP 200 and unauthenticated requests return 401).
3. Traffic is seamlessly cut over to the production port (`8571`) without dropping in-flight requests.
4. `deployed.gr` initiates a background rebuild/swap of the `sticker_item` ASQL Thing. The existing Thing continues serving queries until the new build completes.

---

## Monitoring and Administration

```bash
# Check service status and uptime
app status stickerfeed on <node>

# Follow live service logs
app logs stickerfeed -f on <node>

# Roll back to previous version
app rollback stickerfeed on <node>

# Restart service process
app restart stickerfeed on <node>
```

---

## Troubleshooting

| Symptom | Root Cause | Remediation |
|---|---|---|
| `401` with `Missing Authorization: Bearer header` | Missing `Authorization` header in request | Ensure client provides `Authorization: Bearer <token>`. |
| `401` with `Invalid or expired token` | Token not found, expired, or failed decryption | Verify token in `sticker_feed_exclusions`. If all tokens fail, verify `STICKERFEED_KEY_FILE` or `STICKERFEED_ENCRYPTION_KEY`. |
| `Missing sticker feed encryption key` in logs | Encryption key file and environment variable are both missing | Provision key file at `/var/www/keyfile.txt` or configure secret `STICKERFEED_ENCRYPTION_KEY`, then restart. |
| Deploy fails: `cannot reach ASQL: ...` | Post-deploy hook cannot reach ASQL database | Verify `ASQL_HOST`, `ASQL_PORT`, `ASQL_USER`, and `ASQL_PASSWORD` settings. |
| Initial `/feed` requests take 1–2s | ASQL Thing is compiling initial state | The service dynamically computes feeds across worker threads until `sticker_item` finishes indexing. |
| Timezone discrepancy on `lastsent` timestamps | Host system timezone misconfigured | Set `Environment=TZ=...` in systemd unit or host environment. |

---

## Legacy Compatibility Notes

The legacy PHP feed executed in-band updates to `stockmaster.actualcost` during catalog reads. ASQL enforces strict read/write separation, meaning catalog queries are completely read-only. This eliminates database write locks and ensures consistent response times under heavy concurrent vendor load.
