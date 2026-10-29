// A long read must not take the engine down with it (atl, 2026-10-28: one read held the shared lock for eight
// minutes with replication's writer and every reader queued behind it; its client had long since gone; only a
// restart ended it). Three things, each over the wire with real second connections, as the incident was:
//   - SHOW PROCESSLIST answers at once while a read holds the lock and a writer waits for it (it takes no lock),
//     and KILL QUERY from that operator ends the read;
//   - a client that hangs up mid-statement ends the statement within a second (the hang-up watcher);
//   - a read past AIOS_SQL_MAX_READ_MS ends as a KILLed one does.
// Before the fixes: PROCESSLIST waited the read out, the statement ran to its end after the hang-up, and a read had
// no limit.
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <thread>

#include "aios/kernel/kernel.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/sql_database.hpp"

using namespace aios;
using namespace aios::userland;
using namespace std::chrono_literals;

namespace {
int g_failures = 0;
void check(bool ok, const std::string& label) {
  std::cout << (ok ? "  [ok] " : "  [FAIL] ") << label << "\n";
  if (!ok) ++g_failures;
}
double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
// A read that takes seconds and no index can shorten: every row against every other row (2200 x 2200 pairs, a
// predicate on both sides).
const std::string kLongRead = "SELECT COUNT(*) FROM stall a, stall b WHERE (a.id * 31 + b.id) % 7919 = 3";
const uint16_t kPort = 23397;

struct Client {
  MariaWireSession s{"127.0.0.1", kPort};
  bool open = false;
  Client() { open = s.connect("erp", "stall-check", "aios_db").ok(); }
  // The statement's outcome: ok, or the error text.
  std::string run(const std::string& sql, std::vector<std::vector<std::string>>* rows = nullptr) {
    auto r = s.query(sql);
    if (!r.ok()) return "<" + r.error().message + ">";
    if (!r->ok) return "<" + r->error_message + ">";
    if (rows) {
      rows->clear();
      for (const auto& row : r->rows) {
        std::vector<std::string> cells;
        for (const auto& v : row) cells.push_back(v.is_null ? "NULL" : v.bytes);
        rows->push_back(std::move(cells));
      }
    }
    return "ok";
  }
};
}  // namespace

int main() {
  setenv("AIOS_SQL_MAX_READ_MS", "1500", 1);   // read once, at the first statement
  KernelConfig cfg;
  cfg.root = "/tmp/aios-asql-stall-test";
  auto k = Kernel::boot(cfg);
  if (!k.ok()) { std::printf("kernel boot failed\n"); return 1; }
  SqlDatabase db(**k);
  db.init("sql_db");
  db.execute_local_query("DROP TABLE IF EXISTS stall");
  db.execute_local_query("CREATE TABLE stall (id INT NOT NULL, v VARCHAR(12), PRIMARY KEY(id))");
  {
    std::string sql;
    for (int i = 0; i < 2200; ++i) {
      sql += sql.empty() ? "INSERT INTO stall VALUES " : ",";
      sql += "(" + std::to_string(i) + ",'w" + std::to_string((i * 7919) % 100000) + "')";
      if ((i + 1) % 1000 == 0) { db.execute_local_query(sql); sql.clear(); }
    }
    if (!sql.empty()) db.execute_local_query(sql);
  }
  MysqlWireServer server(**k, db, kPort, "127.0.0.1");
  server.add_account("erp", "stall-check");
  auto sr = server.start();
  check(sr.ok(), "wire server listening on " + std::to_string(kPort));
  server.start_background();

  // How long the read takes alone, with no deadline (it is a nested statement here, not a top-level read).
  {
    const auto t0 = std::chrono::steady_clock::now();
    auto r = db.execute_local_query(kLongRead);
    std::cout << "the long read alone: " << (int)ms_since(t0) << " ms, answer " << (r.rows.empty() ? "?" : r.rows[0].values[0]) << "\n";
    check(r.success && ms_since(t0) > 2500, "the read is long enough to see the effects (over 2.5 s)");
  }

  // 1. PROCESSLIST and KILL from an operator while the read holds the shared lock and a writer waits.
  {
    Client reader, writer, op;
    check(reader.open && writer.open && op.open, "three clients connected");
    auto pr = std::make_shared<std::promise<std::string>>();
    auto read_done = pr->get_future();
    std::thread([&reader, pr] { pr->set_value(reader.run(kLongRead + " AND 2 > 1")); }).detach();   // (not the cached answer above)
    std::this_thread::sleep_for(300ms);
    auto pw = std::make_shared<std::promise<double>>();
    auto write_done = pw->get_future();
    std::thread([&writer, pw] {
      const auto t0 = std::chrono::steady_clock::now();
      writer.run("UPDATE stall SET v = 'x' WHERE v = 'w7919'");   // not by key: needs the lock alone, and waits for the read
      pw->set_value(ms_since(t0));
    }).detach();
    std::this_thread::sleep_for(300ms);
    std::vector<std::vector<std::string>> rows;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string pl = op.run("SHOW PROCESSLIST", &rows);
    const double took = ms_since(t0);
    std::string reader_id;
    for (const auto& row : rows) if (row.size() >= 8 && row[7].find("FROM stall a") != std::string::npos) reader_id = row[0];
    check(pl == "ok" && took < 500, "SHOW PROCESSLIST answered in " + std::to_string((int)took) + " ms with a read holding the lock and a writer waiting");
    check(!reader_id.empty(), "and it lists the read (connection " + reader_id + ")");
    const std::string kr = op.run("KILL QUERY " + (reader_id.empty() ? "0" : reader_id));
    const auto tk = std::chrono::steady_clock::now();
    const std::string rr = read_done.get();
    check(kr == "ok" && rr.find("interrupted") != std::string::npos && ms_since(tk) < 1500,
          "KILL QUERY ended the read in " + std::to_string((int)ms_since(tk)) + " ms: " + rr);
    const double wms = write_done.get();
    check(wms < 3000, "the writer got through after it (" + std::to_string((int)wms) + " ms)");
  }

  // 2. A client hangs up mid-statement.
  {
    auto client = std::make_shared<Client>();
    check(client->open, "client connected");
    auto pq = std::make_shared<std::promise<std::string>>();
    auto qdone = pq->get_future();
    std::thread([client, pq] { pq->set_value(client->run(kLongRead + " AND 3 > 1")); }).detach();
    std::this_thread::sleep_for(400ms);
    client->s.close();   // the browser tab closed
    const auto t0 = std::chrono::steady_clock::now();
    bool ended = false;
    Client op;
    while (ms_since(t0) < 4000) {
      std::vector<std::vector<std::string>> rows;
      op.run("SHOW PROCESSLIST", &rows);
      bool running = false;
      for (const auto& row : rows) if (row.size() >= 8 && row[4] == "Query" && row[7].find("3 > 1") != std::string::npos) running = true;
      if (!running) { ended = true; break; }
      std::this_thread::sleep_for(50ms);
    }
    check(ended && ms_since(t0) < 1000, "the statement ended " + std::to_string((int)ms_since(t0)) + " ms after the client hung up (the read alone takes seconds; the deadline is 1500)");
    qdone.wait_for(5s);
  }

  // 3. The read deadline: a top-level read past AIOS_SQL_MAX_READ_MS (1500 here) ends as KILLed.
  {
    Client c;
    const auto t0 = std::chrono::steady_clock::now();
    const std::string r = c.run(kLongRead + " AND 4 > 1");
    const double took = ms_since(t0);
    check(r.find("interrupted") != std::string::npos && took < 2500, "a read over the deadline ended after " + std::to_string((int)took) + " ms: " + r);
    std::vector<std::vector<std::string>> rows;
    check(c.run("SELECT COUNT(*) FROM stall", &rows) == "ok" && !rows.empty() && rows[0][0] == "2200", "the session is fine after it");
    // The shape that stalled atl: a correlated subquery per outer row (its loop had no check for KILL at all, so
    // the deadline, like KILL QUERY, could not reach it: 7 s against a 1.5 s deadline on the served engine).
    const auto t1 = std::chrono::steady_clock::now();
    const std::string r2 = c.run("SELECT COUNT(*) FROM stall o WHERE (SELECT COUNT(*) FROM stall d WHERE (d.id * 31 + o.id) % 7919 = 3) > 0");
    const double took2 = ms_since(t1);
    check(r2.find("interrupted") != std::string::npos && took2 < 2500, "a correlated-subquery read over the deadline ended after " + std::to_string((int)took2) + " ms: " + r2);
    check(c.run("SELECT 1", &rows) == "ok" && !rows.empty() && rows[0][0] == "1", "SELECT 1 answers (without the statement lock)");
    // Both protections are counted, so an operator can see them act (SHOW STATUS).
    std::vector<std::vector<std::string>> st;
    c.run("SHOW GLOBAL STATUS LIKE 'Asql_read_deadline_kills'", &st);
    const std::string deadline_kills = st.empty() ? "?" : st[0][1];
    c.run("SHOW GLOBAL STATUS LIKE 'Asql_hangup_kills'", &st);
    const std::string hangup_kills = st.empty() ? "?" : st[0][1];
    check(deadline_kills == "2" && hangup_kills == "1", "SHOW STATUS counts them: Asql_read_deadline_kills " + deadline_kills + " (two reads above), Asql_hangup_kills " + hangup_kills + " (one)");
    // A session's own limit: SET max_statement_time (seconds; 0 for no limit) overrides the engine's for that session.
    check(c.run("SET max_statement_time = 0.4") == "ok", "SET max_statement_time = 0.4");
    const auto t2 = std::chrono::steady_clock::now();
    const std::string r3 = c.run(kLongRead + " AND 5 > 1");
    const double took3 = ms_since(t2);
    check(r3.find("interrupted") != std::string::npos && took3 < 1200, "the session's read ended after " + std::to_string((int)took3) + " ms (its own limit, under the engine's 1500)");
    check(c.run("SET SESSION max_statement_time = 0") == "ok", "SET max_statement_time = 0");
    const auto t3 = std::chrono::steady_clock::now();
    const std::string r4 = c.run(kLongRead + " AND 6 > 1");
    check(r4 == "ok" && ms_since(t3) > 2500, "with no limit the read runs to its end (" + std::to_string((int)ms_since(t3)) + " ms): " + r4);
    check(c.run("SET max_statement_time = DEFAULT") == "ok", "SET max_statement_time = DEFAULT");
  }

  // 4. SHOW GLOBAL STATUS while the statement lock is held by someone else (here: taken outright for 3 s, as a
  // long read with a writer waiting holds it): it answers from its counters and the last footprint snapshot.
  {
    Client monitor, reader;
    std::thread holder([&db] { db.rw_lock().lock(); std::this_thread::sleep_for(3s); db.rw_lock().unlock(); });
    std::this_thread::sleep_for(200ms);
    auto pr = std::make_shared<std::promise<double>>();
    auto read_took = pr->get_future();
    std::thread([&reader, pr] { const auto t0 = std::chrono::steady_clock::now(); reader.run("SELECT COUNT(*) FROM stall"); pr->set_value(ms_since(t0)); }).detach();
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::vector<std::string>> rows;
    const std::string r = monitor.run("SHOW GLOBAL STATUS LIKE 'Asql_total_tables'", &rows);
    const double took = ms_since(t0);
    check(r == "ok" && took < 500 && !rows.empty() && rows[0][1] != "0", "SHOW GLOBAL STATUS answered in " + std::to_string((int)took) + " ms with the statement lock held (" + (rows.empty() ? "?" : rows[0][0] + "=" + rows[0][1]) + ")");
    holder.join();
    check(read_took.get() > 2000, "while a read beside it waited for the lock");
  }

  server.stop_background();
  server.stop();
  db.execute_local_query("DROP TABLE stall");
  std::cout << (g_failures ? "FAILED\n" : "PASSED\n");
  return g_failures ? 1 : 0;
}
