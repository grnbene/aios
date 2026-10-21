#!/bin/bash
# Prints the CREATE THING statement for open_orders_view: the grain program in
# open_orders_view.gr, quoted as one SQL string. Pipe it to an ASQL wire port:
#   things/open_orders_view.sh | mysql -h127.0.0.1 -P3307 -uasql summitholding
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
if grep -q '\\' "$here/open_orders_view.gr"; then echo "open_orders_view.gr: no backslashes (they would be read as SQL escapes)" >&2; exit 1; fi
src=$(sed "s/'/''/g" "$here/open_orders_view.gr")
cat <<EOF
CREATE THING open_orders_view GRAIN KEY orderno
  READS salesorders, salesorderdetails, debtortrans, stockmoves, debtorsmaster, custbranch, stockmaster,
        pickingfreeze, locstock, oms_snoozed, salesorderpaymentmethod, trackinginfo, tpl3pl_shipments,
        authorizations, salesordertopurchaseorder, purchorders, suppliers, purchorderdetails, dispatch,
        config, setting, order_status, shippers, locations, taxgroups, taxauthrates, taxgrouptaxes, taxauthorities
  WATCH salesorders(orderno), salesorderdetails(orderno), debtortrans(order_), pickingfreeze(orderno),
        oms_snoozed(orderno), salesorderpaymentmethod(orderno), trackinginfo(transno), tpl3pl_shipments(orderno),
        authorizations(orderno), salesordertopurchaseorder(sales_orderno), dispatch(invoice)
  AS '$src'
EOF
