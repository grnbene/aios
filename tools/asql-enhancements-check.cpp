// Verifies the ASQL kernel enhancements against a booted kernel on a throwaway root.

#undef NDEBUG
#include <iostream>
#include <map>
#include <vector>
#include <cassert>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <chrono>
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/task_ledger.hpp"
#include "aios/storage/aql.hpp"

using namespace aios;
using namespace aios::userland;

int main() {
  std::cout << "=== AIOS ASQL Kernel Enhancements Verification Suite ===\n";

  char root_dir[] = "/tmp/aios-asql-enhancements-XXXXXX";
  if (!mkdtemp(root_dir)) {
    std::perror("mkdtemp");
    return 1;
  }

  KernelConfig cfg;
  cfg.root = root_dir;
  cfg.residency = test_config;
  auto k_res = Kernel::boot(cfg);
  if (!k_res.ok()) {
    std::cerr << "Failed to boot kernel: " << k_res.error().message << "\n";
    return 1;
  }
  Kernel& kernel = *k_res.value();

  TaskLedger ledger(kernel);
  ledger.init("tasks");
  SqlDatabase db(kernel, &ledger);
  db.init("sql_db");

  std::cout << "[Step 1] Multi-Row Bulk Insert via ASQL put_many...\n";
  {
    std::string multi_insert = R"(
      INSERT INTO OBJECTS (KEY, KIND, PAYLOAD) VALUES
        ('sensor:1', 'telemetry', 'val1'),
        ('sensor:2', 'telemetry', 'val2'),
        ('sensor:3', 'telemetry', 'val3'),
        ('sensor:4', 'telemetry', 'val4');
    )";
    auto res = db.execute_gateway_query(multi_insert);
    if (!res.success) {
      std::cerr << "Multi-row insert failed: " << res.error_message << "\n";
      return 1;
    }
    assert(res.rows_affected == 4);
    std::cout << "  -> Multi-row insert inserted 4 objects successfully: " << res.formatted_table << "\n";
  }

  std::cout << "[Step 2] Hex / Binary Literal Syntax (0x... and X'...')...\n";
  {
    std::string hex_insert = "INSERT INTO OBJECTS (KEY, KIND, PAYLOAD) VALUES ('hex:1', 'binary', 0x48656c6c6f20576f726c6421), ('hex:2', 'binary', X'54657374');";
    auto res = db.execute_gateway_query(hex_insert);
    if (!res.success) {
      std::cerr << "Hex insert failed: " << res.error_message << "\n";
      return 1;
    }
    assert(res.rows_affected == 2);

    auto query_res = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"hex:1\"");
    assert(query_res.success);
    assert(query_res.rows.size() == 1);
    // Payload column is index 2
    assert(query_res.rows[0].values[2] == "Hello World!");

    auto query_res2 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"hex:2\"");
    assert(query_res2.success);
    assert(query_res2.rows.size() == 1);
    assert(query_res2.rows[0].values[2] == "Test");
    std::cout << "  -> Hex literals 0x... and X'...' decoded directly to binary payloads: " << query_res.rows[0].values[2] << " & " << query_res2.rows[0].values[2] << "\n";
  }

  std::cout << "[Step 3] Ephemeral Objects & TTL Lifecycle Expiration...\n";
  {
    std::string ttl_insert = "INSERT INTO OBJECTS (KEY, KIND, PAYLOAD, TTL) VALUES ('ephemeral:1', 'cache', 'temp_value', 1);";
    auto res = db.execute_gateway_query(ttl_insert);
    assert(res.success);

    // Immediate lookup: should be found
    auto query_res1 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"ephemeral:1\"");
    assert(query_res1.success);
    assert(query_res1.rows.size() == 1);
    std::cout << "  -> Immediate lookup found ephemeral object: " << query_res1.rows[0].values[0] << "\n";

    // Wait for TTL (1 second) to expire
    std::cout << "  -> Sleeping 1.2s for TTL expiration...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // Post-expiry lookup: should return 0 rows
    auto query_res2 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"ephemeral:1\"");
    assert(query_res2.success);
    assert(query_res2.rows.size() == 0);
    std::cout << "  -> Post-expiry lookup cleanly returned 0 rows (expired).\n";
  }

  std::cout << "[Step 4] Compaction / VACUUM (Tombstones, Superseded Revisions, and Expired Pruning)...\n";
  {
    // Update sensor:1 to a new revision
    auto upd = db.execute_gateway_query("INSERT INTO OBJECTS (KEY, KIND, PAYLOAD) VALUES ('sensor:1', 'telemetry', 'val1_updated');");
    assert(upd.success);

    // Delete sensor:2 via tombstone
    auto del = db.execute_gateway_query("INSERT INTO OBJECTS (KEY, KIND, PAYLOAD, STATUS) VALUES ('sensor:2', 'tombstone', '', '__aios_tombstone__');");
    assert(del.success);

    // Run VACUUM
    auto vac = db.execute_gateway_query("VACUUM OBJECTS;");
    if (!vac.success) {
      std::cerr << "VACUUM failed: " << vac.error_message << "\n";
      return 1;
    }
    std::cout << "  -> VACUUM result table:\n" << vac.formatted_table << "\n";

    // Verify sensor:1 holds updated value
    auto q1 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"sensor:1\"");
    assert(q1.success);
    assert(q1.rows.size() == 1);
    assert(q1.rows[0].values[2] == "val1_updated");

    // Verify sensor:2 is deleted
    auto q2 = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"sensor:2\"");
    assert(q2.success);
    assert(q2.rows.size() == 0);

    // Verify expired item was pruned
    auto q_eph = db.execute_gateway_query("SELECT * FROM objects WHERE meta.key = \"ephemeral:1\"");
    assert(q_eph.success);
    assert(q_eph.rows.size() == 0);
    std::cout << "  -> Post-compaction store parity confirmed!\n";
  }

  std::cout << "[Step 5] Composite Predicate Pushdown & Index Intersection...\n";
  {
    std::string bulk_turns = R"(
      INSERT INTO OBJECTS (KEY, KIND, TURN_STEP, PAYLOAD) VALUES
        ('turn:1', 'agent_turn', '1', 'turn 1 payload'),
        ('turn:2', 'agent_turn', '2', 'turn 2 payload'),
        ('turn:3', 'agent_turn', '3', 'turn 3 payload'),
        ('turn:4', 'agent_turn', '4', 'turn 4 payload'),
        ('turn:5', 'other_kind', '5', 'other 5 payload'),
        ('turn:6', 'agent_turn', '6', 'turn 6 payload');
    )";
    auto res = db.execute_gateway_query(bulk_turns);
    assert(res.success);

    // Query intersecting equality index (kind = 'agent_turn') and range index (turn_step >= 3)
    auto query_res = db.execute_gateway_query("SELECT * FROM objects WHERE meta.kind = \"agent_turn\" AND meta.turn_step >= 3");
    if (!query_res.success) {
      std::cerr << "Composite query failed: " << query_res.error_message << "\n";
      return 1;
    }
    // Matching should be turn:3, turn:4, turn:6 (turn:5 has other_kind, turn:1,2 have step < 3)
    assert(query_res.rows.size() == 3);
    std::cout << "  -> Composite predicate intersection query returned " << query_res.rows.size() << " objects as expected:\n" << query_res.formatted_table << "\n";
  }

  std::cout << "[Step 6] Stored Procedures, Functions, Triggers...\n";
  {
    auto cp = db.execute_gateway_query("CREATE PROCEDURE my_proc(IN x VARCHAR(255), OUT y VARCHAR(255)) BEGIN SET y = x; END");
    assert(cp.success);
    auto cl = db.execute_gateway_query("CALL my_proc('hello_proc', @my_out);");
    assert(cl.success);
    auto sel = db.execute_gateway_query("SELECT @my_out;");
    assert(sel.success && sel.rows.size() == 1 && sel.rows[0].values[0] == "hello_proc");
    std::cout << "  -> Procedure created and CALL returned correct OUT variable.\n";
  }

  std::cout << "[Step 7] Repeated multi-row DELETEs on a big table keep every key pointing at its own row...\n";
  {
    // 200k rows: past the size where DELETE and SELECT go through the column index. The second multi-row DELETE used to
    // compute its erased rows' ids against a half-extended gap list, so every key after it resolved to the wrong row.
    assert(db.execute_gateway_query("CREATE TABLE big_del (id INT PRIMARY KEY, v INT)").success);
    for (int base = 0; base < 200000; base += 20000) {
      std::string ins = "INSERT INTO big_del VALUES ";
      for (int i = base + 1; i <= base + 20000; ++i) ins += std::string(i > base + 1 ? "," : "") + "(" + std::to_string(i) + "," + std::to_string(i % 7) + ")";
      assert(db.execute_gateway_query(ins).success);
    }
    const int ranges[][2] = {{100000, 500}, {110000, 1000}, {120000, 5000}, {130000, 2000}};
    long long expect = 200000;
    for (const auto& r : ranges) {
      const std::string lo = std::to_string(r[0]), hi = std::to_string(r[0] + r[1] - 1);
      auto before = db.execute_gateway_query("SELECT COUNT(*) FROM big_del WHERE id BETWEEN " + lo + " AND " + hi);
      assert(before.success && before.rows[0].values[0] == std::to_string(r[1]));
      assert(db.execute_gateway_query("DELETE FROM big_del WHERE id BETWEEN " + lo + " AND " + hi).success);
      expect -= r[1];
      auto after = db.execute_gateway_query("SELECT COUNT(*) FROM big_del WHERE id BETWEEN " + lo + " AND " + hi);
      assert(after.success && after.rows[0].values[0] == "0");
      auto total = db.execute_gateway_query("SELECT COUNT(*) FROM big_del");
      assert(total.success && total.rows[0].values[0] == std::to_string(expect));
    }
    // A key range nobody touched is still all there, and each key is its own row.
    auto kept = db.execute_gateway_query("SELECT COUNT(*), SUM(id) FROM big_del WHERE id BETWEEN 150000 AND 150009");
    assert(kept.success && kept.rows[0].values[0] == "10" && kept.rows[0].values[1] == "1500045");
    std::cout << "  -> Four multi-row DELETEs left exactly the right rows, each key on its own row.\n";
  }

  std::cout << "[Step 8] A grain Thing keeps exact decimals, reads the wire's text, and re-reads only the keys a write names...\n";
  {
    assert(db.execute_gateway_query("CREATE TABLE gt_items (stockid VARCHAR(20) PRIMARY KEY, price DECIMAL(12,4), qty DECIMAL(12,2))").success);
    assert(db.execute_gateway_query("INSERT INTO gt_items VALUES ('A',0.1,1.10),('B',0.2,2.20),('C',0.0,5.00)").success);
    // read_raw gives every value as the text the wire carries: a price of 0.0000 is the string "0.0000" there (truthy in PHP's
    // rules), and the decimal builtins add it exactly. DAY_SAFE: the Thing names its own wakes, so the day turning reads nothing.
    const std::string create =
        "CREATE THING gt GRAIN KEY stockid READS gt_items WATCH gt_items(stockid) AS '"
        "let DAY_SAFE = true\n"
        "fn enroll() { let out = [] for r in read_raw(\"SELECT stockid FROM gt_items\") { push(out, r.stockid) } return out }\n"
        "fn shared() { return 1 }\n"
        "fn things(ks, sh) { let out = {} for r in read_raw(\"SELECT stockid, price, qty FROM gt_items WHERE stockid IN (?)\", ks) { "
        "out[r.stockid] = {\"sum\": str(decimal(r.price) + decimal(r.qty) + decimal(\"0.1\")), \"txt\": r.price, \"fmt\": format(\"{:.2f}\", decimal(r.price) * 3), \"re\": str(regex_test(\"^[AB]$\", r.stockid))} } return out }\n"
        "'";
    auto made = db.execute_gateway_query(create);
    if (!made.success) std::cerr << made.error_message << "\n";
    assert(made.success);
    auto show = db.execute_gateway_query("SHOW THING gt");
    assert(show.success && show.rows.size() == 3);
    auto col = [&](const SqlQueryResult& r, const char* name) { for (size_t i = 0; i < r.columns.size(); ++i) if (r.columns[i] == name) return i; return (size_t)-1; };
    std::map<std::string, std::vector<std::string>> by_key;
    for (const auto& row : show.rows) by_key[row.values[col(show, "stockid")]] = row.values;
    assert(by_key["A"][col(show, "sum")] == "1.3000");     // 0.1000 + 1.10 + 0.1, exact
    assert(by_key["C"][col(show, "txt")] == "0.0000");     // the wire's text, not the number 0
    assert(by_key["B"][col(show, "fmt")] == "0.60");
    assert(by_key["A"][col(show, "re")] == "true" && by_key["C"][col(show, "re")] == "false");   // regex_* are pure: a Thing may use them
    const std::string vB = by_key["B"][col(show, "version")];
    assert(db.execute_gateway_query("UPDATE gt_items SET price = 0.3 WHERE stockid = 'A'").success);
    show = db.execute_gateway_query("SHOW THING gt");
    assert(show.success && show.rows.size() == 3);
    for (const auto& row : show.rows) by_key[row.values[col(show, "stockid")]] = row.values;
    assert(by_key["A"][col(show, "sum")] == "1.5000");
    assert(by_key["A"][col(show, "version")] == "2");      // the key the write named was read again
    assert(by_key["B"][col(show, "version")] == vB);       // the others were not
    // KEYS: only the Things asked for, in the order asked, matched as the collation matches ('c' is C); none for a key
    // that is no Thing, one row for a key named twice
    auto some = db.execute_gateway_query("SHOW THING gt KEYS ('c', 'A', 'nope', 'a ')");
    if (!some.success) std::cerr << some.error_message << "\n";
    assert(some.success && some.rows.size() == 2);
    assert(some.rows[0].values[col(some, "stockid")] == "C" && some.rows[1].values[col(some, "stockid")] == "A");
    assert(some.rows[1].values[col(some, "sum")] == "1.5000");
    assert(db.execute_gateway_query("SHOW THING gt KEYS ()").rows.empty());
    assert(!db.execute_gateway_query("SHOW THING gt KEYS 'A'").success);
    // A full read is the rows kept for the kind's version: a Thing that goes, or a new one, is in the next read.
    assert(db.execute_gateway_query("DELETE FROM gt_items WHERE stockid = 'C'").success);
    show = db.execute_gateway_query("SHOW THING gt");
    assert(show.success && show.rows.size() == 2);
    assert(db.execute_gateway_query("INSERT INTO gt_items VALUES ('D',1.0,1.00)").success);
    show = db.execute_gateway_query("SHOW THING gt");
    assert(show.success && show.rows.size() == 3);
    by_key.clear();
    for (const auto& row : show.rows) by_key[row.values[col(show, "stockid")]] = row.values;
    assert(by_key.count("D") && by_key["D"][col(show, "sum")] == "2.1000" && !by_key.count("C"));
    // Full reads beside writes: each read is whole (three Things, every sum one the table held)
    {
      std::atomic<bool> stop{false};
      std::atomic<int> reads{0}, bad{0};
      std::vector<std::thread> readers;
      for (int t = 0; t < 4; ++t)
        readers.emplace_back([&] {
          while (!stop) {
            auto r = db.execute_gateway_query("SHOW THING gt");
            if (!r.success || r.rows.size() != 3) bad++;
            reads++;
          }
        });
      for (int i = 0; i < 40; ++i)
        assert(db.execute_gateway_query("UPDATE gt_items SET qty = " + std::to_string(i) + " WHERE stockid = 'B'").success);
      while (reads < 40) std::this_thread::yield();
      stop = true;
      for (auto& th : readers) th.join();
      assert(bad == 0);
      show = db.execute_gateway_query("SHOW THING gt");
      for (const auto& row : show.rows) by_key[row.values[col(show, "stockid")]] = row.values;
      assert(by_key["B"][col(show, "sum")] == "39.3000");   // 0.2 + 39 + 0.1: the last write, not a kept older read
    }
    std::cout << "  -> A grain Thing added decimals exactly, kept the wire's text, and a write moved only its own key.\n";
  }

  std::cout << "[Step 9] DELETE ... MATCHING removes one row per image, NULLs and text as = compares them...\n";
  {
    assert(db.execute_gateway_query("CREATE TABLE km_log (at DATETIME, who VARCHAR(20), what TEXT)").success);
    assert(db.execute_gateway_query("INSERT INTO km_log VALUES ('2026-01-01 00:00:00', 'ann', 'x'), ('2026-01-01 00:00:00', 'ann', 'x'), "
                                    "('2026-01-01 00:00:00', 'ann', 'x'), ('2026-01-02 00:00:00', NULL, 'y'), ('2026-01-03 00:00:00', 'Bob', 'z')").success);
    auto d = db.execute_gateway_query("DELETE FROM km_log MATCHING ('2026-01-01 00:00:00', 'ann', 'x'), ('2026-01-02 00:00:00', NULL, 'y'), "
                                      "('2026-01-03 00:00:00', 'BOB ', 'z'), ('2026-01-09 00:00:00', 'nobody', 'q')");
    if (!d.success || d.rows_affected != 3) std::cerr << "MATCHING: " << d.success << " " << d.rows_affected << " " << d.error_message << "\n";
    assert(d.success && d.rows_affected == 3);              // one of three duplicates, the NULL row, Bob by =; the fourth names no row
    auto left = db.execute_gateway_query("SELECT COUNT(*) FROM km_log WHERE who = 'ann'");
    assert(left.success && left.rows.size() == 1 && left.rows[0].values[0] == "2");
    assert(db.execute_gateway_query("SELECT COUNT(*) FROM km_log").rows[0].values[0] == "2");
    assert(!db.execute_gateway_query("DELETE FROM km_log MATCHING ('2026-01-01 00:00:00', 'ann')").success);   // a value per column
    std::cout << "  -> MATCHING deleted exactly the rows named, one per image.\n";
  }

  std::cout << "[Step 9b] A re-encode made off the lock answers as before, and gives way to a write that came between...\n";
  {
    assert(db.execute_gateway_query("CREATE TABLE oe_items (id INT PRIMARY KEY, name VARCHAR(40), price DECIMAL(10,2), qty INT)").success);
    for (int b = 0; b < 10; ++b) {
      std::string ins = "INSERT INTO oe_items VALUES ";
      for (int i = 0; i < 500; ++i) {
        const int id = b * 500 + i;
        ins += std::string(i ? "," : "") + "(" + std::to_string(id) + ", 'item-" + std::to_string(id % 777) + "', " +
               std::to_string(id % 913) + "." + std::to_string(id % 100 < 10 ? 5 : id % 100) + ", " + std::to_string(id % 37) + ")";
      }
      assert(db.execute_gateway_query(ins).success);
    }
    const std::vector<std::string> probes = {
        "SELECT COUNT(*), SUM(price), SUM(qty), MIN(name), MAX(name), COUNT(DISTINCT name) FROM oe_items",
        "SELECT name, price FROM oe_items WHERE id IN (0, 7, 499, 500, 4999)",
        "SELECT COUNT(*) FROM oe_items WHERE price BETWEEN 100 AND 200 AND name LIKE 'item-1%'",
        "SELECT qty, COUNT(*) FROM oe_items GROUP BY qty ORDER BY qty LIMIT 5"};
    auto answers = [&] {
      std::vector<std::string> out;
      for (const auto& q : probes) {
        auto r = db.execute_gateway_query(q);
        assert(r.success);
        std::string a;
        for (const auto& row : r.rows) for (const auto& v : row.values) a += v + "|";
        out.push_back(a);
      }
      return out;
    };
    const auto before = answers();
    SqlTable* t = db.find_table("oe_items");
    assert(t);
    std::vector<ColumnStore> cols;
    uint64_t seq = 0;
    assert(t->take_encoding_copy(&cols, &seq));
    SqlTable::encode_columns(cols, t->columns);
    assert(t->install_encoding(cols, seq));
    assert(answers() == before);                                     // the new encoding reads the same rows
    // a write between the copy and the install: the copy is refused, the write stays
    cols.clear();
    assert(t->take_encoding_copy(&cols, &seq));
    assert(db.execute_gateway_query("UPDATE oe_items SET price = 1234.56 WHERE id = 7").success);
    SqlTable::encode_columns(cols, t->columns);
    assert(!t->install_encoding(cols, seq));
    auto p7 = db.execute_gateway_query("SELECT price FROM oe_items WHERE id = 7");
    assert(p7.success && p7.rows.size() == 1 && p7.rows[0].values[0] == "1234.56");
    std::cout << "  -> Encoded beside the table, the columns went in only when nothing had written it meanwhile.\n";
  }

  std::cout << "[Step 10] A predicate's per-value verdicts outlive an UPDATE in place, and stay right...\n";
  {
    assert(db.execute_gateway_query("CREATE TABLE kv_t (id INT PRIMARY KEY, v VARCHAR(10))").success);
    std::string ins = "INSERT INTO kv_t VALUES ";
    for (int i = 0; i < 3000; ++i) ins += (i ? ", (" : "(") + std::to_string(i) + ", '" + std::string(1, (char)('a' + i % 3)) + "')";
    assert(db.execute_gateway_query(ins).success);
    auto count = [&](const char* where) {
      auto r = db.execute_gateway_query(std::string("SELECT COUNT(*) FROM kv_t WHERE ") + where);
      assert(r.success && r.rows.size() == 1);
      return std::stol(r.rows[0].values[0]);
    };
    assert(count("v > 'b'") == 1000);                                              // the c's
    assert(db.execute_gateway_query("UPDATE kv_t SET v = 'zz' WHERE id = 0").success);   // an a becomes a value never seen
    assert(count("v > 'b'") == 1001);
    assert(db.execute_gateway_query("UPDATE kv_t SET v = 'a' WHERE id = 2").success);    // a c becomes an a
    assert(count("v > 'b'") == 1000);
    assert(count("v = 'zz'") == 1 && count("v = 'a'") == 1000);
    assert(db.execute_gateway_query("UPDATE kv_t SET v = 'c' WHERE id = 0").success);    // the only zz goes
    assert(count("v > 'b'") == 1000 && count("v = 'zz'") == 0 && count("v = 'c'") == 1000);
    // deletes, a GROUP BY over the column, and a value whose last row goes and comes back (by INSERT, by UPDATE)
    auto groups = [&]() {
      auto r = db.execute_gateway_query("SELECT v, COUNT(*) FROM kv_t GROUP BY v ORDER BY v");
      assert(r.success);
      std::string out;
      for (const auto& row : r.rows) out += row.values[0] + "=" + row.values[1] + " ";
      return out;
    };
    assert(groups() == "a=1000 b=1000 c=1000 ");
    assert(db.execute_gateway_query("UPDATE kv_t SET v = 'q' WHERE id = 1").success);   // one q
    assert(groups() == "a=1000 b=999 c=1000 q=1 ");
    assert(db.execute_gateway_query("DELETE FROM kv_t WHERE id = 1").success);          // the q goes: its code has no row
    assert(groups() == "a=1000 b=999 c=1000 " && count("v > 'b'") == 1000);
    assert(db.execute_gateway_query("DELETE FROM kv_t WHERE id IN (3, 6, 9)").success);
    assert(groups() == "a=997 b=999 c=1000 ");
    assert(db.execute_gateway_query("UPDATE kv_t SET v = 'q' WHERE id = 4").success);   // q again, by UPDATE
    assert(groups() == "a=997 b=998 c=1000 q=1 " && count("v > 'b'") == 1001);
    assert(db.execute_gateway_query("DELETE FROM kv_t WHERE id = 4").success);
    assert(db.execute_gateway_query("INSERT INTO kv_t VALUES (1, 'q')").success);      // and by INSERT
    assert(groups() == "a=997 b=998 c=1000 q=1 " && count("v = 'q'") == 1);
    std::cout << "  -> Counts followed every UPDATE and DELETE.\n";
  }

  std::cout << "[Step 11] CREATE OR REPLACE THING builds the new version beside the old and takes its name...\n";
  {
    assert(db.execute_gateway_query("CREATE TABLE rp_items (id VARCHAR(10) PRIMARY KEY, n INT)").success);
    assert(db.execute_gateway_query("INSERT INTO rp_items VALUES ('a', 1), ('b', 2)").success);
    auto make = [&](const char* verb, const char* factor) {
      return std::string(verb) + " THING rp GRAIN KEY id READS rp_items AS '"
             "fn enroll() { let out = [] for r in read(\"SELECT id FROM rp_items\") { push(out, r.id) } return out }\n"
             "fn shared() { return 1 }\n"
             "fn things(ks, sh) { let out = {} for r in read(\"SELECT id, n FROM rp_items WHERE id IN (?)\", ks) { out[r.id] = {\"v\": r.n * " + factor + "} } return out }\n'";
    };
    auto made = db.execute_gateway_query(make("CREATE", "10"));
    if (!made.success) std::cerr << made.error_message << "\n";
    assert(made.success);
    auto v_of = [&](const std::string& key) {
      auto r = db.execute_gateway_query("SHOW THING rp");
      assert(r.success);
      size_t ci = 0, vi = 0;
      for (size_t i = 0; i < r.columns.size(); ++i) { if (r.columns[i] == "id") ci = i; if (r.columns[i] == "v") vi = i; }
      for (const auto& row : r.rows) if (row.values[ci] == key) return row.values[vi];
      return std::string("?");
    };
    assert(v_of("a") == "10" && v_of("b") == "20");
    auto again = db.execute_gateway_query(make("CREATE", "100"));
    assert(!again.success);                                  // plain CREATE still refuses a name in use
    auto rep = db.execute_gateway_query(make("CREATE OR REPLACE", "100"));
    if (!rep.success) std::cerr << rep.error_message << "\n";
    assert(rep.success);
    assert(v_of("a") == "100" && v_of("b") == "200");        // the new program's values
    assert(db.execute_gateway_query("UPDATE rp_items SET n = 3 WHERE id = 'a'").success);
    assert(v_of("a") == "300");                              // and it follows writes as the Thing
    auto things = db.execute_gateway_query("SHOW THINGS");
    for (const auto& row : things.rows) assert(row.values[0] != "rp~next");   // the builder is gone
    std::cout << "  -> Replaced in place, new values, still live.\n";
  }

  std::cout << "\n=== All ASQL Kernel Enhancements Verified Successfully ===\n";
  return 0;
}
