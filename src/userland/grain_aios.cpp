// grain natives that only exist inside aios: the sidecar's C ABI (crypto,
// documents, geometry, capabilities), the SAT solver, the MariaDB wire client
// and the in-process ASQL engine. The portable standard library is
// grain_builtins.cpp (install_core_builtins); a grain built without aios links
// that alone, and aios's own VMs get both through install_builtins().
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "aios/sidecar/aios_sidecar_c_abi.h"
#include "aios/userland/grain.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/sat_solver.hpp"
#include "aios/userland/sql_sat.hpp"
#include "grain_builtins_internal.hpp"

namespace aios::userland::grain {

namespace {

// --- AIOS Sidecar C-ABI Native Bindings ------------------------------------

Value bi_sidecar_haversine(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_number(args[0], error, "sidecar_haversine") ||
      !want_number(args[1], error, "sidecar_haversine") ||
      !want_number(args[2], error, "sidecar_haversine") ||
      !want_number(args[3], error, "sidecar_haversine")) return Value::nil();
  double dist = aios_haversine_distance(args[0].as_double(), args[1].as_double(),
                                        args[2].as_double(), args[3].as_double());
  return Value::real(dist);
}

Value bi_sidecar_tsp(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_tsp")) return Value::nil();
  const std::string& s = args[0].str();
  std::vector<double> coords;
  size_t start = 0;
  while (start < s.size()) {
    size_t semi = s.find(';', start);
    if (semi == std::string::npos) semi = s.size();
    std::string pair = s.substr(start, semi - start);
    size_t comma = pair.find(',');
    if (comma != std::string::npos) {
      coords.push_back(std::strtod(pair.substr(0, comma).c_str(), nullptr));
      coords.push_back(std::strtod(pair.substr(comma + 1).c_str(), nullptr));
    }
    start = semi + 1;
  }
  int count = (int)coords.size() / 2;
  if (count < 2) {
    error = "sidecar_tsp needs at least 2 coordinate pairs (lat,lng;...)";
    return Value::nil();
  }
  std::vector<int> order(count);
  double total_dist = 0;
  if (aios_tsp_solve_2opt(coords.data(), count, order.data(), &total_dist) != AIOS_OK) {
    error = "sidecar_tsp failed to solve route";
    return Value::nil();
  }
  std::vector<Value> out_items;
  for (int idx : order) out_items.push_back(Value::integer(idx));
  return Value::list(std::move(out_items));
}

Value bi_sidecar_sha256(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_sha256")) return Value::nil();
  const std::string& s = args[0].str();
  uint8_t digest[32];
  aios_crypto_sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size(), digest);
  static const char* hex_digits = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (int i = 0; i < 32; ++i) {
    hex.push_back(hex_digits[digest[i] >> 4]);
    hex.push_back(hex_digits[digest[i] & 0xF]);
  }
  return Value::string(hex);
}

Value bi_sidecar_keccak256(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_keccak256")) return Value::nil();
  const std::string& s = args[0].str();
  uint8_t digest[32];
  aios_crypto_keccak256(reinterpret_cast<const uint8_t*>(s.data()), s.size(), digest);
  static const char* hex_digits = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (int i = 0; i < 32; ++i) {
    hex.push_back(hex_digits[digest[i] >> 4]);
    hex.push_back(hex_digits[digest[i] & 0xF]);
  }
  return Value::string(hex);
}

Value bi_sidecar_eth_addr(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_eth_addr")) return Value::nil();
  std::string hex = args[0].str();
  if (hex.rfind("0x", 0) == 0) hex = hex.substr(2);
  if (hex.size() != 64) { error = "sidecar_eth_addr wants 32-byte (64-char) hex private key"; return Value::nil(); }
  uint8_t priv[32];
  for (size_t i = 0; i < 32; ++i) {
    char h[3] = {hex[2*i], hex[2*i+1], 0};
    priv[i] = static_cast<uint8_t>(std::strtoul(h, nullptr, 16));
  }
  char addr[64] = {0};
  if (aios_chain_eth_derive_address(priv, addr, sizeof(addr)) != AIOS_OK) {
    error = "sidecar_eth_addr derivation failed";
    return Value::nil();
  }
  return Value::string(addr);
}

Value bi_sidecar_btc_addr(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_btc_addr")) return Value::nil();
  std::string hex = args[0].str();
  if (hex.size() != 66) { error = "sidecar_btc_addr wants 33-byte (66-char) compressed pubkey hex"; return Value::nil(); }
  uint8_t pubkey[33];
  for (size_t i = 0; i < 33; ++i) {
    char h[3] = {hex[2*i], hex[2*i+1], 0};
    pubkey[i] = static_cast<uint8_t>(std::strtoul(h, nullptr, 16));
  }
  // A flag: true or a nonzero number. Grain's own truthiness calls 0 true, which made a mainnet request (0) a testnet address.
  int testnet = args.size() > 1 && (args[1].type == Type::Bool ? args[1].truthy() : args[1].as_int() != 0) ? 1 : 0;
  char addr[128] = {0};
  if (aios_chain_btc_p2wpkh_address(pubkey, testnet, addr, sizeof(addr)) != AIOS_OK) {
    error = "sidecar_btc_addr derivation failed";
    return Value::nil();
  }
  return Value::string(addr);
}

Value bi_sidecar_parse_eml(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_parse_eml")) return Value::nil();
  std::vector<char> buf(65536, 0);
  if (aios_comms_parse_eml_json(args[0].str().c_str(), buf.data(), buf.size()) != AIOS_OK) {
    error = "sidecar_parse_eml failed to parse RFC 5322 MIME email";
    return Value::nil();
  }
  return Value::string(buf.data());
}

Value bi_sidecar_parse_ical(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_parse_ical")) return Value::nil();
  std::vector<char> buf(65536, 0);
  if (aios_comms_parse_ical_json(args[0].str().c_str(), buf.data(), buf.size()) != AIOS_OK) {
    error = "sidecar_parse_ical failed to parse iCalendar";
    return Value::nil();
  }
  return Value::string(buf.data());
}

Value bi_sidecar_parse_vcard(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_parse_vcard")) return Value::nil();
  std::vector<char> buf(65536, 0);
  if (aios_comms_parse_vcard_json(args[0].str().c_str(), buf.data(), buf.size()) != AIOS_OK) {
    error = "sidecar_parse_vcard failed to parse vCard";
    return Value::nil();
  }
  return Value::string(buf.data());
}

Value bi_sidecar_parse_feed(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_parse_feed")) return Value::nil();
  std::vector<char> buf(131072, 0);
  if (aios_comms_parse_rss_json(args[0].str().c_str(), buf.data(), buf.size()) != AIOS_OK) {
    error = "sidecar_parse_feed failed to parse XML feed";
    return Value::nil();
  }
  return Value::string(buf.data());
}

Value bi_sidecar_svg_png_len(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_svg_png_len")) return Value::nil();
  int w = args.size() > 1 && args[1].is_number() ? (int)args[1].as_int() : 128;
  int h = args.size() > 2 && args[2].is_number() ? (int)args[2].as_int() : 128;
  std::vector<uint8_t> png_buf(512 * 1024);
  size_t out_len = png_buf.size();   // capacity in, length out (0 in was "buffer too small")
  if (aios_svg_render_png(args[0].str().c_str(), w, h, 0xFF000000, png_buf.data(), &out_len) != AIOS_OK) {
    error = "sidecar_svg_png_len failed to render SVG";
    return Value::nil();
  }
  return Value::integer((int64_t)out_len);
}

Value bi_sidecar_pdf_len(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_pdf_len")) return Value::nil();
  std::string css = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : "";
  std::vector<uint8_t> pdf_buf(1024 * 1024);
  size_t out_len = pdf_buf.size();   // capacity in, length out (0 in was "buffer too small")
  if (aios_pdf_render_html(args[0].str().c_str(), css.c_str(), 800, pdf_buf.data(), &out_len) != AIOS_OK) {
    error = "sidecar_pdf_len failed to render PDF";
    return Value::nil();
  }
  return Value::integer((int64_t)out_len);
}

Value bi_sidecar_pdf_render(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_pdf_render")) return Value::nil();
  std::string css = args.size() > 1 && args[1].type == Type::Str ? args[1].str() : "";
  int vp_w = 800;
  if (args.size() > 2 && args[2].is_number()) vp_w = (int)args[2].as_int();
  std::vector<uint8_t> pdf_buf(2 * 1024 * 1024);
  size_t out_len = pdf_buf.size();
  int rc = aios_pdf_render_html(args[0].str().c_str(), css.c_str(), vp_w, pdf_buf.data(), &out_len);
  if (rc == AIOS_ERR_BUFFER_TOO_SMALL) {
    pdf_buf.resize(out_len);
    rc = aios_pdf_render_html(args[0].str().c_str(), css.c_str(), vp_w, pdf_buf.data(), &out_len);
  }
  if (rc != AIOS_OK) {
    error = "sidecar_pdf_render failed to render PDF";
    return Value::nil();
  }
  pdf_buf.resize(out_len);
  return Value::object(Type::Bytes, std::make_shared<BytesObj>(std::move(pdf_buf)));
}

Value bi_sidecar_cap_mint(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_cap_mint") ||
      !want_str(args[1], error, "sidecar_cap_mint") ||
      !want_str(args[2], error, "sidecar_cap_mint") ||
      !want_number(args[3], error, "sidecar_cap_mint")) return Value::nil();
  char cap_id[AIOS_CAP_ID_LEN + 1] = {0};
  if (aios_cap_mint(args[0].str().c_str(), args[1].str().c_str(), args[2].str().c_str(),
                    static_cast<uint16_t>(args[3].as_int()), cap_id, sizeof(cap_id)) != AIOS_OK) {
    error = "sidecar_cap_mint failed";
    return Value::nil();
  }
  return Value::string(cap_id);
}

Value bi_sidecar_cap_attenuate(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_cap_attenuate") ||
      !want_number(args[1], error, "sidecar_cap_attenuate")) return Value::nil();
  char child_cap[AIOS_CAP_ID_LEN + 1] = {0};
  if (aios_cap_attenuate(args[0].str().c_str(), static_cast<uint16_t>(args[1].as_int()),
                         child_cap, sizeof(child_cap)) != AIOS_OK) {
    error = "sidecar_cap_attenuate failed";
    return Value::nil();
  }
  return Value::string(child_cap);
}

Value bi_sidecar_patch_check(Vm&, std::vector<Value>& args, std::string& error) {
  if (!want_str(args[0], error, "sidecar_patch_check")) return Value::nil();
  int hunks = 0, files = 0, can_apply = 0;
  if (aios_patch_check_dry_run(args[0].str().c_str(), &hunks, &files, &can_apply) != AIOS_OK) {
    error = "sidecar_patch_check failed";
    return Value::nil();
  }
  return Value::integer(can_apply ? (int64_t)hunks : -1);
}

// sat(clauses [, conflict_limit]): the aios SAT solver, its answer verified
// before it comes back (userland/sql_sat.hpp). clauses: a list of lists of
// non-zero ints, or DIMACS text. Deterministic - a Thing may call it. UNSAT
// comes with "core": which of the clauses cannot all hold together.
Value bi_sat(Vm&, std::vector<Value>& args, std::string& error) {
  std::vector<std::vector<int>> clauses;
  int vars = 0;
  if (args[0].type == Type::Str) {
    std::string err;
    if (!aios::userland::SatSolver::parse_dimacs(args[0].str(), &vars, &clauses, &err)) { error = "sat(): " + err; return Value::nil(); }
  } else if (args[0].type == Type::List) {
    for (const Value& c : args[0].list_items()) {
      if (c.type != Type::List) { error = "sat(): each clause is a list of literals"; return Value::nil(); }
      std::vector<int> cl;
      for (const Value& l : c.list_items()) {
        if (!l.is_number() || l.as_int() == 0) { error = "sat(): a literal is a non-zero int"; return Value::nil(); }
        const int64_t v = l.as_int();
        if (v > 0x3fffffff || v < -0x3fffffff) { error = "sat(): variable out of range"; return Value::nil(); }
        cl.push_back((int)v);
        vars = std::max<int>(vars, (int)(v > 0 ? v : -v));
      }
      clauses.push_back(std::move(cl));
    }
  } else {
    error = "sat(): clauses as a list of lists, or DIMACS text";
    return Value::nil();
  }
  uint64_t limit = 0;
  if (args.size() > 1) {
    if (!want_number(args[1], error, "sat")) return Value::nil();
    limit = (uint64_t)std::max<int64_t>(0, args[1].as_int());
  }
  const auto a = aios::userland::solve_verified(vars, clauses, limit);
  auto m = std::make_shared<MapObj>();
  m->put("status", Value::string(a.status == 10 ? "SAT" : a.status == 20 ? "UNSAT" : "UNKNOWN"));
  m->put("verified", Value::boolean(a.verified));
  m->put("evidence", Value::string(a.evidence));
  m->put("conflicts", Value::integer((int64_t)a.conflicts));
  if (a.status == 10) {
    std::vector<Value> model;
    model.reserve(a.model.size());
    for (int l : a.model) model.push_back(Value::integer(l));
    m->put("model", Value::list(std::move(model)));
  }
  if (a.status == 20) {
    // why: the fewest of the given clauses that cannot all hold (their indices in `clauses`, from 0), checked
    std::vector<Value> core;
    for (size_t c : a.core) core.push_back(Value::integer((int64_t)c));
    m->put("core", Value::list(std::move(core)));
    m->put("core_verified", Value::boolean(a.core_verified));
    m->put("core_evidence", Value::string(a.core_evidence));
  }
  return Value::object(Type::Map, m);
}

// --- Database Connectivity & Wire Client ------------------------------------

static std::shared_ptr<aios::userland::MariaWireSession> get_db_session(const Value& val) {
  if (val.type == Type::Extern) {
    auto ext = std::static_pointer_cast<ExternObj>(val.obj);
    if (ext->kind_name == "db_connection") {
      return std::static_pointer_cast<aios::userland::MariaWireSession>(ext->ptr);
    }
  } else if (val.type == Type::Map) {
    auto map = std::static_pointer_cast<MapObj>(val.obj);
    Value* c = map->find("conn");
    if (c && c->type == Type::Extern) {
      auto ext = std::static_pointer_cast<ExternObj>(c->obj);
      if (ext->kind_name == "db_connection") {
        return std::static_pointer_cast<aios::userland::MariaWireSession>(ext->ptr);
      }
    }
  }
  return nullptr;
}

Value bi_db_connect(Vm&, std::vector<Value>& args, std::string& error) {
  if (!db_connect_args(args, error)) return Value::nil();
  if (args.size() < 4) {
    error = "db_connect wants (host, port, user, pass, [database])";
    return Value::nil();
  }
  std::string host = args[0].to_text();
  int port = (int)args[1].as_int();
  if (port <= 0) port = 3306;
  std::string user = args[2].to_text();
  std::string pass = args[3].to_text();
  std::string db = args.size() > 4 ? args[4].to_text() : "";

  auto session = std::make_shared<aios::userland::MariaWireSession>(host, (uint16_t)port);
  auto conn_res = session->connect(user, pass, db);
  auto map = std::make_shared<MapObj>();
  if (!conn_res.ok()) {
    map->put("ok", Value::boolean(false));
    map->put("error", Value::string(conn_res.error().message));
    return Value::object(Type::Map, map);
  }

  auto conn_obj = std::make_shared<ExternObj>("db_connection", session);
  map->put("ok", Value::boolean(true));
  map->put("conn", Value::object(Type::Extern, conn_obj));
  map->put("server_version", Value::string(session->server_version()));
  map->put("thread_id", Value::integer((int64_t)session->thread_id()));
  map->put("host", Value::string(host));
  map->put("port", Value::integer(port));
  map->put("user", Value::string(user));
  map->put("database", Value::string(db));
  return Value::object(Type::Map, map);
}

Value bi_db_query(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.size() < 2) {
    error = "db_query wants (conn, sql)";
    return Value::nil();
  }
  auto session = get_db_session(args[0]);
  if (!session) {
    error = "db_query wants a valid db_connection handle";
    return Value::nil();
  }

  std::string sql = args[1].to_text();
  auto q_res = session->query(sql);

  auto out = std::make_shared<MapObj>();
  if (!q_res.ok()) {
    out->put("ok", Value::boolean(false));
    out->put("error", Value::string(q_res.error().message));
    out->put("columns", Value::list({}));
    out->put("column_types", Value::list({}));
    out->put("rows", Value::list({}));
    out->put("affected_rows", Value::integer(0));
    out->put("last_insert_id", Value::integer(0));
    out->put("message", Value::string(""));
    return Value::object(Type::Map, out);
  }

  const auto& mres = q_res.value();
  out->put("ok", Value::boolean(mres.ok));
  out->put("error", Value::string(mres.ok ? "" : mres.error_message));
  out->put("affected_rows", Value::integer((int64_t)mres.affected_rows));
  out->put("last_insert_id", Value::integer((int64_t)mres.last_insert_id));
  out->put("message", Value::string(mres.message));

  std::vector<Value> cols;
  std::vector<Value> col_types;
  for (const auto& col : mres.columns) {
    cols.push_back(Value::string(col.name));
    col_types.push_back(Value::string(col.type.empty() ? "VARCHAR" : col.type));
  }
  out->put("columns", Value::list(cols));
  out->put("column_types", Value::list(col_types));

  std::vector<Value> rows;
  for (const auto& r : mres.rows) {
    std::vector<Value> row_cells;
    for (const auto& cell : r) {
      if (cell.is_null) {
        row_cells.push_back(Value::nil());
      } else {
        row_cells.push_back(Value::string(cell.bytes));
      }
    }
    rows.push_back(Value::list(row_cells));
  }
  out->put("rows", Value::list(rows));
  return Value::object(Type::Map, out);
}

Value bi_db_select_db(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.size() < 2) { error = "db_select_db wants (conn, db_name)"; return Value::nil(); }
  auto session = get_db_session(args[0]);
  if (!session) { error = "db_select_db wants a valid db_connection"; return Value::nil(); }
  auto res = session->select_db(args[1].to_text());
  return Value::boolean(res.ok());
}

Value bi_db_ping(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "db_ping wants (conn)"; return Value::nil(); }
  auto session = get_db_session(args[0]);
  if (!session) { error = "db_ping wants a valid db_connection"; return Value::nil(); }
  auto res = session->ping();
  return Value::boolean(res.ok());
}

Value bi_db_close(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "db_close wants (conn)"; return Value::nil(); }
  auto session = get_db_session(args[0]);
  if (session) session->close();
  return Value::nil();
}

Value bi_db_connected(Vm&, std::vector<Value>& args, std::string&) {
  if (args.empty()) return Value::boolean(false);
  auto session = get_db_session(args[0]);
  return Value::boolean(session && session->connected());
}

Value bi_db_tables(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "db_tables wants (conn, [db_name])"; return Value::nil(); }
  auto session = get_db_session(args[0]);
  if (!session) { error = "db_tables wants a valid db_connection"; return Value::nil(); }
  if (args.size() > 1 && !args[1].to_text().empty()) {
    session->select_db(args[1].to_text());
  }
  auto q = session->query("SHOW FULL TABLES;");
  std::vector<Value> tables;
  if (q.ok()) {
    for (const auto& row : q.value().rows) {
      if (row.empty()) continue;
      auto m = std::make_shared<MapObj>();
      m->put("name", Value::string(row[0].bytes));
      m->put("type", Value::string(row.size() > 1 ? row[1].bytes : "BASE TABLE"));
      tables.push_back(Value::object(Type::Map, m));
    }
  }
  return Value::list(tables);
}

Value bi_db_columns(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.size() < 2) { error = "db_columns wants (conn, table_name)"; return Value::nil(); }
  auto session = get_db_session(args[0]);
  if (!session) { error = "db_columns wants a valid db_connection"; return Value::nil(); }
  std::string tbl = args[1].to_text();
  auto q = session->query("SHOW FULL COLUMNS FROM `" + tbl + "`;");
  std::vector<Value> cols;
  if (q.ok()) {
    for (const auto& row : q.value().rows) {
      if (row.size() < 6) continue;
      auto m = std::make_shared<MapObj>();
      m->put("name", Value::string(row[0].bytes));
      m->put("type", Value::string(row[1].bytes));
      m->put("nullable", Value::boolean(row[3].bytes == "YES"));
      m->put("key", Value::string(row[4].bytes));
      m->put("is_pk", Value::boolean(row[4].bytes == "PRI"));
      m->put("default", row[5].is_null ? Value::nil() : Value::string(row[5].bytes));
      m->put("extra", row.size() > 6 ? Value::string(row[6].bytes) : Value::string(""));
      cols.push_back(Value::object(Type::Map, m));
    }
  }
  return Value::list(cols);
}

Value bi_db_schema(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "db_schema wants (conn)"; return Value::nil(); }
  auto session = get_db_session(args[0]);
  if (!session) { error = "db_schema wants a valid db_connection"; return Value::nil(); }

  auto tables_q = session->query("SHOW FULL TABLES;");
  auto schema_map = std::make_shared<MapObj>();
  auto tbls_map = std::make_shared<MapObj>();
  std::vector<Value> tbl_names;
  std::vector<Value> all_cols;
  std::unordered_set<std::string> col_set;

  if (tables_q.ok()) {
    for (const auto& row : tables_q.value().rows) {
      if (row.empty()) continue;
      std::string tbl_name = row[0].bytes;
      tbl_names.push_back(Value::string(tbl_name));

      auto col_q = session->query("SHOW COLUMNS FROM `" + tbl_name + "`;");
      std::vector<Value> tbl_cols;
      if (col_q.ok()) {
        for (const auto& crow : col_q.value().rows) {
          if (crow.empty()) continue;
          std::string cname = crow[0].bytes;
          tbl_cols.push_back(Value::string(cname));
          if (col_set.find(cname) == col_set.end()) {
            col_set.insert(cname);
            all_cols.push_back(Value::string(cname));
          }
        }
      }
      tbls_map->put(tbl_name, Value::list(tbl_cols));
    }
  }

  schema_map->put("tables", Value::object(Type::Map, tbls_map));
  schema_map->put("table_names", Value::list(tbl_names));
  schema_map->put("all_columns", Value::list(all_cols));
  return Value::object(Type::Map, schema_map);
}

Value bi_db_build_update(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.size() < 4) {
    error = "db_build_update wants (table_name, pk_col, pk_val, dirty_map)";
    return Value::nil();
  }
  std::string table = args[0].to_text();
  std::string pk_col = args[1].to_text();
  std::string pk_val = args[2].to_text();
  if (args[3].type != Type::Map) {
    error = "db_build_update dirty_map must be a Map";
    return Value::nil();
  }
  auto dirty_map = static_cast<MapObj*>(args[3].obj.get());
  if (dirty_map->entries.empty()) return Value::string("");

  std::string sql = "UPDATE `" + table + "` SET ";
  bool first = true;
  for (const auto& entry : dirty_map->entries) {
    if (!first) sql += ", ";
    first = false;
    std::string val = entry.second.to_text();
    std::string escaped;
    for (char c : val) {
      if (c == '\'') escaped += "''";
      else if (c == '\\') escaped += "\\\\";
      else escaped += c;
    }
    sql += "`" + entry.first + "` = '" + escaped + "'";
  }
  sql += " WHERE `" + pk_col + "` = '" + pk_val + "';";
  return Value::string(sql);
}

// Who asql_query / asql_exec / thing_conflicts sign in as: root with no password unless their options map says
// ({user, password, database}) - an engine started with a password file wants it.
struct AsqlLogin { std::string user = "root", password, database; };
AsqlLogin asql_login(const std::vector<Value>& args, size_t at) {
  AsqlLogin l;
  if (args.size() <= at || args[at].type != Type::Map || !args[at].obj) return l;
  auto m = std::dynamic_pointer_cast<MapObj>(args[at].obj);
  if (!m) return l;
  if (Value* v = m->find("user")) l.user = v->to_text();
  if (Value* v = m->find("password")) l.password = v->to_text();
  if (Value* v = m->find("database"); v || (v = m->find("db"))) l.database = v->to_text();
  return l;
}

// thing_conflicts(kind, [key], [port=3307], [{user, password, database}]): a SOURCE BY Thing's conflicts, one entry
// per conflicted field - {key, field, taken: source or nil, by: "hand"|"rule" when taken, sides: [{source, value, taken}]} - from
// SHOW THING CONFLICTS (userland/sql_things.hpp).
Value bi_thing_conflicts(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty() || args[0].type != Type::Str) { error = "thing_conflicts wants (kind, [key], [port], [{user, password, database}])"; return Value::nil(); }
  std::string sql = "SHOW THING CONFLICTS " + args[0].str();
  size_t at = 1;
  if (args.size() > at && args[at].type == Type::Str) {
    std::string k = args[at].str(), q;
    for (char c : k) { if (c == '\'') q += '\''; q += c; }
    sql += " '" + q + "'";
    ++at;
  }
  int port = 3307;
  if (args.size() > at && args[at].is_number()) port = (int)args[at++].as_int();
  const AsqlLogin login = asql_login(args, at);
  auto session = std::make_shared<aios::userland::MariaWireSession>("127.0.0.1", (uint16_t)port);
  auto conn = session->connect(login.user, login.password, login.database);
  if (!conn.ok()) { error = "thing_conflicts: cannot connect to 127.0.0.1:" + std::to_string(port) + ": " + conn.error().message; return Value::nil(); }
  auto q = session->query(sql);
  if (!q.ok()) { error = "thing_conflicts: " + q.error().message; return Value::nil(); }
  const auto& r = q.value();
  if (!r.ok) { error = "thing_conflicts: " + r.error_message; return Value::nil(); }
  int ck = -1, cf = -1, cs = -1, cv = -1, ct = -1, ch = -1;
  for (size_t i = 0; i < r.columns.size(); ++i) {
    const std::string& n = r.columns[i].name;
    if (n == "key") ck = (int)i; else if (n == "field") cf = (int)i; else if (n == "source") cs = (int)i;
    else if (n == "value") cv = (int)i; else if (n == "taken") ct = (int)i; else if (n == "how") ch = (int)i;
  }
  if (ck < 0 || cf < 0 || cs < 0 || cv < 0 || ct < 0) { error = "thing_conflicts: unexpected answer from SHOW THING CONFLICTS"; return Value::nil(); }
  std::vector<std::pair<std::string, std::shared_ptr<MapObj>>> order;   // (key\x1ffield) -> entry, as first seen
  for (const auto& row : r.rows) {
    auto cell = [&](int c) { return row[(size_t)c].is_null ? std::string() : row[(size_t)c].bytes; };
    const std::string id = cell(ck) + "\x1f" + cell(cf);
    std::shared_ptr<MapObj> e;
    for (auto& o : order) if (o.first == id) e = o.second;
    if (!e) {
      e = std::make_shared<MapObj>();
      e->put("key", Value::string(cell(ck)));
      e->put("field", Value::string(cell(cf)));
      e->put("taken", Value::nil());
      e->put("sides", Value::list({}));
      order.emplace_back(id, e);
    }
    auto side = std::make_shared<MapObj>();
    side->put("source", Value::string(cell(cs)));
    side->put("value", row[(size_t)cv].is_null ? Value::nil() : Value::string(cell(cv)));
    const bool taken = cell(ct) == "1";
    side->put("taken", Value::boolean(taken));
    if (taken) {
      e->put("taken", Value::string(cell(cs)));
      e->put("by", ch >= 0 && !row[(size_t)ch].is_null ? Value::string(cell(ch)) : Value::string("hand"));   // "hand" or "rule"
    }
    std::vector<Value> sides = e->find("sides")->list_items();
    sides.push_back(Value::object(Type::Map, side));
    e->put("sides", Value::list(std::move(sides)));
  }
  std::vector<Value> out;
  for (auto& o : order) out.push_back(Value::object(Type::Map, o.second));
  return Value::list(std::move(out));
}

Value bi_asql_query(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "asql_query wants (sql, [port=3307], [{user, password, database}])"; return Value::nil(); }
  std::string sql = args[0].to_text();
  int port = args.size() > 1 && args[1].is_number() ? (int)args[1].as_int() : 3307;
  const AsqlLogin login = asql_login(args, 2);

  // Only the port asked for. This used to fall back to 3306 - a real
  // MariaDB on hosts that run one - so a query meant for ASQL could land in a
  // different database, as root, without a word.
  auto session = std::make_shared<aios::userland::MariaWireSession>("127.0.0.1", (uint16_t)port);
  auto conn_res = session->connect(login.user, login.password, login.database);
  if (!conn_res.ok()) {
    error = "asql_query: cannot connect to 127.0.0.1:" + std::to_string(port) + ": " + conn_res.error().message;
    return Value::nil();
  }
  auto q_res = session->query(sql);
  if (!q_res.ok()) {
    error = "asql_query: " + q_res.error().message;
    return Value::nil();
  }
  const auto& mres = q_res.value();
  if (!mres.ok) {
    error = "asql_query: " + mres.error_message;
    return Value::nil();
  }
  std::vector<Value> row_maps;
  for (const auto& r : mres.rows) {
    auto m = std::make_shared<MapObj>();
    for (size_t i = 0; i < mres.columns.size() && i < r.size(); ++i) {
      if (r[i].is_null) m->put(mres.columns[i].name, Value::nil());
      else m->put(mres.columns[i].name, Value::string(r[i].bytes));
    }
    row_maps.push_back(Value::object(Type::Map, m));
  }
  return Value::list(std::move(row_maps));
}

Value bi_asql_exec(Vm&, std::vector<Value>& args, std::string& error) {
  if (args.empty()) { error = "asql_exec wants (sql, [port=3307], [{user, password, database}])"; return Value::nil(); }
  std::string sql = args[0].to_text();
  int port = args.size() > 1 && args[1].is_number() ? (int)args[1].as_int() : 3307;
  const AsqlLogin login = asql_login(args, 2);

  // Only the port asked for. This used to fall back to 3306 - a real
  // MariaDB on hosts that run one - so a query meant for ASQL could land in a
  // different database, as root, without a word.
  auto session = std::make_shared<aios::userland::MariaWireSession>("127.0.0.1", (uint16_t)port);
  auto conn_res = session->connect(login.user, login.password, login.database);
  if (!conn_res.ok()) {
    error = "asql_exec: cannot connect to 127.0.0.1:" + std::to_string(port) + ": " + conn_res.error().message;
    return Value::nil();
  }
  auto q_res = session->query(sql);
  auto out = std::make_shared<MapObj>();
  if (!q_res.ok()) {
    out->put("ok", Value::boolean(false));
    out->put("error", Value::string(q_res.error().message));
    return Value::object(Type::Map, out);
  }
  const auto& mres = q_res.value();
  out->put("ok", Value::boolean(mres.ok));
  out->put("error", Value::string(mres.ok ? "" : mres.error_message));
  out->put("affected_rows", Value::integer((int64_t)mres.affected_rows));
  out->put("last_insert_id", Value::integer((int64_t)mres.last_insert_id));
  return Value::object(Type::Map, out);
}

}  // namespace

void install_aios_builtins(Vm& vm) {
  vm.define_native("sat", 1, 2, bi_sat);
  vm.define_native("sidecar_haversine", 4, 4, bi_sidecar_haversine);
  vm.define_native("sidecar_tsp", 1, 1, bi_sidecar_tsp);
  vm.define_native("sidecar_sha256", 1, 1, bi_sidecar_sha256);
  vm.define_native("sidecar_keccak256", 1, 1, bi_sidecar_keccak256);
  vm.define_native("sidecar_eth_addr", 1, 1, bi_sidecar_eth_addr);
  vm.define_native("sidecar_btc_addr", 1, 2, bi_sidecar_btc_addr);
  vm.define_native("sidecar_parse_eml", 1, 1, bi_sidecar_parse_eml);
  vm.define_native("sidecar_parse_ical", 1, 1, bi_sidecar_parse_ical);
  vm.define_native("sidecar_parse_vcard", 1, 1, bi_sidecar_parse_vcard);
  vm.define_native("sidecar_parse_feed", 1, 1, bi_sidecar_parse_feed);
  vm.define_native("sidecar_svg_png_len", 1, 3, bi_sidecar_svg_png_len);
  vm.define_native("sidecar_pdf_len", 1, 2, bi_sidecar_pdf_len);
  vm.define_native("sidecar_pdf_render", 1, 3, bi_sidecar_pdf_render);
  vm.define_native("sidecar_cap_mint", 4, 4, bi_sidecar_cap_mint);
  vm.define_native("sidecar_cap_attenuate", 2, 2, bi_sidecar_cap_attenuate);
  vm.define_native("sidecar_patch_check", 1, 1, bi_sidecar_patch_check);
  vm.define_native("db_connect", 1, 5, bi_db_connect);
  vm.define_native("db_query", 2, 2, bi_db_query);
  vm.define_native("db_select_db", 2, 2, bi_db_select_db);
  vm.define_native("db_ping", 1, 1, bi_db_ping);
  vm.define_native("db_close", 1, 1, bi_db_close);
  vm.define_native("db_connected", 1, 1, bi_db_connected);
  vm.define_native("db_tables", 1, 2, bi_db_tables);
  vm.define_native("db_columns", 2, 2, bi_db_columns);
  vm.define_native("db_schema", 1, 1, bi_db_schema);
  vm.define_native("db_build_update", 4, 4, bi_db_build_update);
  vm.define_native("asql_query", 1, 3, bi_asql_query);
  vm.define_native("asql_exec", 1, 3, bi_asql_exec);
  vm.define_native("thing_conflicts", 1, 4, bi_thing_conflicts);
}

void install_builtins(Vm& vm) {
  install_core_builtins(vm);
  install_aios_builtins(vm);
}

}  // namespace aios::userland::grain
