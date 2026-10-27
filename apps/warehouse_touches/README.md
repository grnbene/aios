# Warehouse Touches Service

Warehouse Touches is an AIOS web service written in Grain that aggregates warehouse operations and generates multi-format reporting for ERP users.

It replaces the legacy PHP endpoint (`touches.php`) while querying the primary operations tables directly from ASQL.

Related documentation:
- [Running and managing Grain applications](../../docs/grain-apps.md)
- [Writing Grain applications](../../docs/grain-app-writing.md)
- [Hosting applications with AIOS](../../docs/aios-serve.md)

---

## API Endpoints

| Endpoint | Method | Response | Description |
|---|---|---|---|
| `/` or `/quickreportview` | `GET` | HTML | Interactive dashboard (summary cards, touch mix bars, ranked operator table, and date filter) |
| `/quickreport` | `GET` | JSON | Aggregated touch rollups and per-operator metrics |
| `/quickreportpdf` | `GET` | `application/pdf` | Vector PDF report in landscape letter format |
| `/quickreportxlsx` | `GET` | `application/vnd.openxmlformats-officedocument...` | OpenXML spreadsheet export with frozen panes and cell styling |
| `/quickreportcsv` | `GET` | `text/csv` | Tabular CSV export covering all 31 metrics |
| `/health` | `GET` | JSON | Health status (`{"status":"ok"}`) |

### Query Parameters

All reporting endpoints accept optional date filter parameters:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `start` | `YYYY-MM-DD` | 30 days ago | Beginning of the date range |
| `stop` | `YYYY-MM-DD` | Current date | End of the date range |
| `range` | `START..STOP` | *(none)* | Combined date span string (e.g. `2026-09-01..2026-10-26`) |
| `userid` | string | *(none)* | Filter activity to a single operator ID |

---

## Architecture and File Layout

```text
apps/warehouse_touches/
├── app.gr                 # Application manifest (port 8573, listen interface, settings, secrets)
├── service.gr             # HTTP routes and request handlers
├── lib/
│   ├── queries.gr         # ASQL touch queries (recscan, picking, bin movements, loads, stickers)
│   └── views.gr           # Document and view renderers (HTML dashboard, PDF, XLSX, CSV)
└── README.md              # Service documentation
```

### Warehouse Operations ("Touches")

The service queries five distinct operational areas from ASQL:
1. **Receiving (`recscan`)**: Pallet and carton receipts, quantities, scan counts, and vendor tallies.
2. **Picking (`pickingfreeze` + `stockmaster`)**: Merchandise picks excluding non-inventory freight/fees.
3. **Bin Movements (`binmovements_details`)**: Stock movements, merges, and dolly relocations.
4. **Truck Loading (`loadingbays` + `dispatch`)**: Dispatches, loading scan counts, manual overrides, and driver times.
5. **Sticker Scans (`stickerscanout` + `salesorderdetails`)**: Pack and stage unit scans and bound orders.

---

## Host Configuration

Configuration settings are set through `app set` or environment variables:

```bash
app set warehouse_touches \
    DB_HOST=127.0.0.1 \
    DB_PORT=3307 \
    DB_USER=root \
    DB_NAME=versioneight

# Pass MariaDB/ASQL password via stdin
cat /opt/aios/mysql.pass | app secret warehouse_touches DB_PASSWORD
```

### Settings Reference

| Setting | Default | Description |
|---|---|---|
| `DB_HOST` | `127.0.0.1` | ASQL / database host |
| `DB_PORT` | `3307` | ASQL / database port |
| `DB_USER` | `root` | Database user |
| `DB_NAME` | `versioneight` | Database name |
| `WAREHOUSE_COMPUTE_ON` | `""` | Optional fleet member ID to route report computation to |

### Secrets Reference

| Secret | Description |
|---|---|
| `DB_PASSWORD` | Password for the database connection |

---

## Running and Verification

To run standalone with the Grain CLI:

```bash
grain run apps/warehouse_touches/service.gr
```
