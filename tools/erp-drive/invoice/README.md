# Invoicing drive

Runs the v8 ERP's own `Weberp\\B2b::createInvoice` (from a checkout of v8-server, `V8=/path/to/v8-server/`) against
a scratch ASQL: the production schema subset (`../../proof/asql/openorders-schema.sql`, with the ERP's auto-increment
ids), the tables invoicing writes besides (`extra.sql`), and 40 picked orders over three items (`seed-base.sql`,
`seed-orders.py`).

    python3 seed-orders.py > orders.sql
    ./up.sh 33500                       # a fresh engine on :33500 (AIOS_TXN_CONCURRENT=1 in the environment for that mode)
    php drive.php 33500 1 LOG=1         # one invoice, every statement it sent
    php drive.php 33500 2 "FAIL_AT=insert into stockmoves"   # fails at that statement: what is left behind
    ./clicks.sh 33500                   # one order invoiced by several clicks: exactly one invoice
    ./many.sh 33500                     # 40 invoices by 8 workers: the books checked
    ./chaos.sh 33500                    # ... every fifth failing part way, while a POS worker sells the same items
    ./lockcost.sh 33500 500             # ... with a 0.5 ms round trip per statement: how long a POS sale waits
    php rollback.php 33500              # ROLLBACK takes back the transaction's changes and nothing else
    php concurrency.php 33500           # the concurrent-transaction rules, over real connections

`up.sh` expects the schema, `extra.sql`, `seed-base.sql` and `orders.sql` beside it (edit the paths at its top).
What it found and what changed in the engine: docs/asql.md, "Transactions".

`createInvoice-atomic.patch` is the change to the ERP's `system/library/weberp/b2b.php` these drives were run
against (apply in a v8-server checkout with `git am` or `git apply`):

- The invoice number is taken before `START TRANSACTION`. `getNextTransNo` commits whatever is open, so called
  inside, it committed the invoice and nothing after it could roll back.
- `catch (\Throwable)` instead of `catch (Exception)`. In `namespace Weberp` that named a class that does not
  exist, so the rollback, `invoice.log` and the deadlock retries never ran.
- The order is claimed by compare-and-swap on `salesorders.invoiceno`, keyed by `orderno` alone. Without back
  orders, an order that already has an invoice is refused inside the transaction, so extra clicks post nothing.
- A deadlock (1213) or lock wait timeout (1205) retries the whole invoice, not one statement.
- Lines are processed in stock-code order, so two invoices take `locstock` rows in the same order.
- The quantity-on-hand note is read without `FOR UPDATE`.
