# Shipped Thing Declarations

This directory contains standalone SQL reference declarations and proof-harness Thing definitions shipped with the ASQL distribution package.

> [!NOTE]
> For the comprehensive user and architecture guide on Things (covering IVM, `WHEN ... DO`, `AS OF`, `EXPLAIN THING`, `WHAT IF`, and `INTO`), see [docs/things.md](../docs/things.md).
> For engine internals and grammar reference, see [docs/asql.md](../docs/asql.md#things).

---

## Directory Contents

| File | Type | Description |
|---|---|---|
| `open_orders.sql` | ASQL Declaration | Reference declaration tracking open orders, open credits, and invoiced dispatch orders per `orderno`. Packaged into `/opt/aios/things/`. |
| `open_orders_parts.sql` | ASQL Declaration | Reference declaration demonstrating `PART` child queries (`trans`, `lines`) per order. Packaged into `/opt/aios/things/`. |
| `picked_orders.sql` | ASQL Declaration | Reference active Thing demonstrating `WHEN ... DO ... TO <profile>`, advancing orders to Picked (21) when picking completes. Packaged into `/opt/aios/things/`. |
| `open_orders_view.gr` | Grain Script | Grain-based Thing implementation computing full open order views across multiple tables. |
| `open_orders_view.sh` | Shell Wrapper | Wraps `open_orders_view.gr` into a quoted `CREATE THING ... GRAIN` statement for wire execution and test harnesses (e.g. `tools/proof/asql/openorders-drive.sh`). |

---

## Packaging and Deployment

* **ASQL Distribution**: [packaging/pack.sh](../packaging/pack.sh) bundles `*.sql` and this manifest into the package bundle. [packaging/install-asql.sh](../packaging/install-asql.sh) installs them to `/opt/aios/things/` for operators to declare against ASQL via the console or standard MySQL client.
* **Testing and Verification**: [tools/proof/asql/openorders-drive.sh](../tools/proof/asql/openorders-drive.sh) executes `open_orders_view.sh` against throwaway test instances.

---

## Application Things

Grain Things that belong to specific services are maintained inside their respective application packages rather than this top-level directory:
* `apps/stickerfeed/things/sticker_item.gr`
* `apps/warehouse_touches/things/touch_hour.gr`

These are packaged and registered automatically during application deployment (`app deploy`) through each app's `deployed.gr` hook.
