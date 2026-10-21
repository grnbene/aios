// Proves MariaWireSession - the client already wired into SqlDatabase's
// remote-profile path - against a genuinely INDEPENDENT external MariaDB,
// not AIOS's own MysqlWireServer. aios-mysql-check only ever connects it
// back to this codebase's own server, which proves protocol symmetry, not
// real-world interop; this is the test that was missing.
#include "aios/userland/mysql_wire.hpp"
#include <cstdio>

int main() {
  aios::userland::MariaWireSession session("127.0.0.1", 3306);
  auto c = session.connect("aios_test", "aios_test_pw_2026", "aios_client_test");
  if (!c.ok()) {
    printf("CONNECT FAILED: %s\n", c.error().message.c_str());
    return 1;
  }
  printf("CONNECTED to real external MariaDB. server_version=%s\n", session.server_version().c_str());

  auto ping = session.ping();
  printf("PING: ok=%d err=%s\n", ping.ok(), ping.error().message.c_str());

  auto sel = session.query("SELECT id, name FROM t1 ORDER BY id");
  if (!sel.ok()) {
    printf("SELECT FAILED: %s\n", sel.error().message.c_str());
    return 1;
  }
  printf("SELECT: ok=%d has_result_set=%d columns=%zu rows=%zu\n",
         sel->ok, sel->has_result_set, sel->columns.size(), sel->rows.size());
  for (auto& row : sel->rows) {
    printf("  row:");
    for (auto& v : row) printf(" %s", v.is_null ? "NULL" : v.bytes.c_str());
    printf("\n");
  }

  auto bad = session.query("SELECT * FROM nonexistent_table_xyz");
  printf("BAD QUERY: ok=%d err=%s\n", bad.ok() ? (*bad).ok : false,
         bad.ok() ? (*bad).error_message.c_str() : bad.error().message.c_str());

  session.close();
  return 0;
}
