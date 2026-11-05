# km_data: KM data API

A read-only REST API over KM Canada East's ERP data (the `km` database), for KM's SQL Server / Power BI
loads. Every read goes to the ASQL replica of `km`. Live MariaDB carries no query load; it only feeds the
replica through its binlog.

```text
km MariaDB (live) --binlog--> ASQL replica (aar, 127.0.0.1:3307) --> km_data (grain, 127.0.0.1:8595) --> nginx (HTTPS) --> KM
```

## Endpoints

All need `Authorization: Bearer <key>` except `/health`.

| Path | What |
|---|---|
| `GET /v1/schema` | every entity, its key, filters, fields and types |
| `GET /v1/lookups` | warehouses, payment terms, price lists, categories, brands, areas, territories, hold reasons, customer types, ship via, currencies, transaction types |
| `GET /v1/<entity>` | one page of an entity, in key order |
| `GET /v1/sales_by_salesperson?from=&to=` | net sales (invoices less credits, before tax and freight) per rep per day; at most 1,096 days (3 years) |
| `GET /health` | whether the replica is reachable and how far behind it is |

Entities: `customers`, `customer_locations`, `customer_contacts`, `customer_notes`, `sales_orders`, `sales_order_lines`, `invoices`, `credit_notes`,
`payments`, `customer_transactions`, `invoice_lines`, `allocations`, `receivables`, `salespeople`, `vendors`,
`vendor_transactions`, `items`, `item_prices`, `inventory_levels`, `inventory_movements`, `purchase_orders`,
`purchase_order_lines`, `receipts`.

Parameters on an entity:

- `limit` (1-50000, default 5000) and `after` (the `next` of the previous page). Paging is keyset: rows come in
  key order and `next` is null on the last page, so pages never repeat or skip rows.
- `from` / `to` (YYYY-MM-DD) on entities with a date (`date_filter` in the schema).
- Equality filters listed per entity in the schema (`customer_id`, `sku`, `order_id`, ...).
- `format=csv`: the page as CSV; the cursor is in the `X-Next` header, the row count in `X-Count`.

An unknown parameter is refused (400), so a typo never returns everything.

Values: ids are text, money has 2 decimals (CAD), rates are fractions (`0.10` = 10%; `1` on a sales line is a
free-goods line), dates are `YYYY-MM-DD`, date-times `YYYY-MM-DDTHH:MM:SS` (Eastern). The ERP's zero dates
(`0000-00-00`) are `null`.

`receivables` is every open customer item (total less applied not zero), with `days_overdue` and
`aging_bucket` (`not_due`, `0_29`, `30_59`, `60_89`, `90_plus`) worked out on today's date against the due
date, as the ERP's aged analysis does. `customers` carries what the ERP's Customer Info card shows: `balance`, `lifetime_invoiced` and first and
last invoice and receipt dates from the ASQL Thing `kmdata_customer_balance`; total spend, purchase dates, monthly
averages and return rate from the Thing `kmdata_customer_sales`; sales MTD / YTD / last year YTD / last year / last
12 months from the Thing `kmdata_customer_periods`; aging buckets that add up to `balance` from the Thing
`kmdata_customer_aging`; last payment, average invoice and top three brands over the last 365 days, the main
ship-to's rep and contact, open order and quote counts, last web login, reward points and clubs, worked out per page.
The full 5,000-customer page takes 0.5 s on full km (4.1 s when periods and aging were worked out per page). `sales_orders` carries the order's line totals and the subtotal, shipping,
tax and total saved with it. `allocations` names both documents of each application (type, number, date).

## Where each figure comes from in the ERP

Every computed figure follows the v8 server code the ERP screens use (paths under `v8/server/catalog/`), and was
compared with that code's own SQL run on the same data (km copy to 2026-10-30):

| API | ERP source | Compared |
|---|---|---|
| `receivables` open items and `aging_bucket` | `model/cli/statement.php` getAgedAnalysisBatch (open = total - alloc, aged on duedate) | all six bucket totals equal to the cent |
| `customers.ledger_balance` | the same function's Balance column (inner joins paymentterms, so a document with an unmatched terms code is left out) | equal on all 5,444 customers ($312,249.32) |
| `customers.balance` | sum of the open items above | equals the receivables total ($313,341.61) |
| `invoice_lines.line_amount` | the stocktrans view's rounding (`migrations/stocktrans_decimal_line_math.sql`); printed lines only (`show_on_inv_crds = 1`); a write-off credit shows its positive line only (`controller/apir/documents/credits.php`) | lines add to the header subtotal on 2,454 of 2,459 documents (Sep-Oct); the 5 others are 1-5 cent header rounding |
| `sales_by_salesperson` | the km `stocktrans` view, which the ERP's sales reports read (`model/reports/report.php` getSRPT / getSPSLS); reproduced join for join, since ASQL replicates tables, not views. As the view does, it leaves out lines whose item has no preferred purchdata row or whose preferred supplier does not exist (ZZZSWITCHOUT switch-out credits, blank-SKU lines) and counts an item once per brand row | September 2026 on aar's replica vs live `stocktrans`: all 13 reps equal to the cent on sales, cost and units ($649,844.91); October checked the same way on live km |
| `customers.total_spend`, `avg_monthly_sales`, `avg_monthly_units`, `return_rate`, `last_purchase_date` | the card's Total Spend / Avg Monthly Sales / Qty / Return % / Last Purchase (`controller/apir/customers.php` getcustomer, `model/weberp/customer.php`), which read `stocktrans`: printed type 10/11 lines with their ledger document, the view's item exclusions and line rounding. Two of the view's joins are left out so the Thing reads only per-customer tables: the item/supplier/brand joins (a Thing reading them is rebuilt whole, 5-10 s inside the replicating write, on any item or cost change) and the ledger join's fan-out (`R0001` document 565595 has 62 ledger rows, so the view counts each of its 62 lines 62 times) | customer 00048 on the 10-30 copy: $84,578.26, equal to the view's joins; Thing reacts to a ledger change (one invoice unhooked: -$1,517.50, restored) |
| `customers.last_receipt_amount`, `avg_invoice_last_365`, `top_brand_1..3` | `customer.php` getLastPaid, getAvgSpend, getTopThreeBrandsAsArray | same expressions |
| `sales_orders.subtotal` / `shipping` / `tax` / `total` | `salesorders.payment_details`, saved from the cart's totals (`controller/apir/cart.php`), what Order Inquiry shows; `open_subtotal` is the Open Orders list's basis (`model/oms/orders.php` getOrderTotals) | order 1667135: lines $1,517.50 = saved subtotal = invoice 1803123 |
| `items.unit_cost` | `model/weberp/items.php` getItem / getValuation (material + labour + overhead, else lastcost) | same expression |
| `items.qty_on_hand` | `items.php` getItemStock (no transfer warehouse, no PO holding bin) | valuation report counts the holding bin too: $10,398.70 / 2,235 units on ZPOREQ, the whole difference |
| `items.qty_on_open_orders` | `items.php` getItemDemand | equal, less 14 units on order lines with a blank SKU |
| `items.qty_on_open_pos` | `system/library/weberp/b2b.php` getOnOpenPO | same expression |
| `purchase_orders.status` | `po_status` by `status_id` (the text `status` column drifts: 42 POs say Cancelled with status 100) | |
| `sales_orders.invoice_note` / `dispatch_note` | `salesorders.comments` is printed on the KM invoice; `salesorders.notes` on the KM picking slip | |

Known ERP data quirks the API shows as they are: 45 invoices whose `ovgst` differs from their
`debtortranstaxes` rows (the printed KM invoice prints the latter); credit notes' `ovdiscount` (the printed credit
leaves it out, the ledger keeps it); accounts 101236 and 15951, whose allocation records do not add up, so
`balance` and `ledger_balance` differ there.

## Keys

A key must be both:

1. an active row of `km.apikeys` (`active = 1`, `` `database` = KMDATA_KEY_DB ``), so it can be switched off in
   the ERP, and
2. listed in this app's `KMDATA_KEYS` secret, so no other ERP key reaches the data.

`<key>_<userid>` tokens are read as their key, as the ERP's `validate.php` does. Every call is logged
(`app logs km_data`): when, the key's `assigned` name, path, status, rows, ms. Keys are never logged.

## Settings

| Setting | Default | |
|---|---|---|
| `ASQL_HOST` / `ASQL_PORT` / `ASQL_USER` / `ASQL_DB` | `127.0.0.1` / `3307` / `erp` / `km` | the replica |
| `KMDATA_KEY_DB` | `km` | the `apikeys.database` value a key must carry |

Secrets: `ASQL_PASSWORD`, `KMDATA_KEYS` (comma-separated keycodes).

## On aar

1. Add `apikeys` to `CDC_TABLES` in `/etc/aios/asql.conf` and restart `aios-compute`, so keys replicate.
2. Run an app host (`aios-apps.service`, `serve port=7480`), then:
   ```bash
   app deploy apps/km_data
   cat /opt/aios/mysql.pass | app secret km_data ASQL_PASSWORD
   printf '%s' '<keycode>' | app secret km_data KMDATA_KEYS
   ```
3. nginx: proxy a path on the API's HTTPS host to `127.0.0.1:8595`.

`deployed.gr` creates the `kmdata_customer_balance`, `kmdata_customer_sales`, `kmdata_customer_periods` and
`kmdata_customer_aging` Things (`lib/things.gr`) on first deploy and leaves them in place after; one whose definition
changed is rebuilt (on full km: sales about 6 s, periods 4.4 s, aging 3.3 s). Periods and aging read `CURDATE()`, so
ASQL works each out whole once a new day starts, and per customer on every change in between. On the full km copy a
single stockmoves write for a customer took about 0.3 s more with the periods Thing (its ledger check reads
debtortrans whole); a debtortrans write on the cash account 99999 (263,000 rows) about 0.1 s more with aging.

On aar, `CDC_TABLES` must also carry `config`, `po_status`, `product_types`, `quotes`, `quote_details`, `customer_login`,
`loyaltytrans` and `clubs` (besides `apikeys`): items, inventory_levels, purchase_orders, lookups, customers and
customer_clubs read them, and aar did not replicate them on 2026-11-05.
