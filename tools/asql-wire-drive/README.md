# asql-wire-drive

Drives one MySQL-wire server through the client paths the ERP and its tools
actually use, and prints one line per check. Run it against MariaDB and
against ASQL on the same data and `diff` the two outputs: every line that
differs is a divergence to explain or fix.

    mysql < setup-wdrv.sql                      # reference data into MariaDB
    mysqldump wdrv > wdrv.sql
    mysql -h127.0.0.1 -P<asql-port> -e 'CREATE DATABASE wdrv'
    mysql -h127.0.0.1 -P<asql-port> wdrv < wdrv.sql   # into ASQL, through the wire
    php drive-wire.php 127.0.0.1 3306 <user> <pass>              > maria.txt
    php drive-wire.php 127.0.0.1 <asql-port> root ''             > asql.txt
    php drive-wire.php localhost 0 root '' /path/to/asql.sock    > asql-sock.txt
    diff maria.txt asql.txt

What it covers (PHP mysqli with mysqlnd, the ERP's client): text protocol on
a miss and a cached hit, column metadata, prepared statements (binary
protocol) on repeat and on other parameters, bind_result, aggregates,
send_long_data, cache freshness after an UPDATE on both protocols,
multi-statements including an error mid-reply, utf8mb4 and a latin1 session,
CHAR_LENGTH on every evaluation path, SHOW CREATE TABLE, error codes and
sqlstates, COM_RESET / change_user / persistent `p:` connections, ping,
COM_STATISTICS, a 20,000-row join, a 17 MB cell, a 20 MB REPEAT.

Known, accepted differences as of 2026-09-30: INT columns are typed
LONGLONG (the engine keeps no declared width), text charset 33 vs 45, a
latin1 session is honoured only under `mysql_charset=1`, SHOW CREATE TABLE
says varchar(255), error message wording, and user variables (`SET @v`) are
unsupported (the ERP has none).
