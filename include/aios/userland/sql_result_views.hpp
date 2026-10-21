#pragma once
// Result views: a report's whole answer kept in the engine until a table it
// reads changes, handed out as finished JSON or as one page of it.
//
//   CALL VIEW '<options JSON>' SELECT ...
//   CALL VIEW STATUS
//
// Options (all optional):
//   "text":       "utf8" (default), "latin1" or "latin1_iso" - both latin1 modes
//                 start from the bytes a latin1 session reads; "latin1" takes
//                 them as UTF-8 when they are (murken's double-encoded columns
//                 come out right), "latin1_iso" as ISO-8859-1, exactly what
//                 PHP's utf8_encode() - and so PhpReports' pages - show
//   "gzip":       true - the payload is one gzip member
//   "if_version": a version from an earlier answer - status 'not_modified'
//                 and no payload when the rows are still the same
//   "draw":       echoed back (DataTables' request counter)
//   "window":     {"start":0, "length":100 (-1 = all),
//                  "order":[[column index, "asc"|"desc"], ...],
//                  "search":"words"}   - every word must be in some cell
//
// The answer is one row per <= 1 MB piece of the payload, in order:
//   status, version, rows_total, rows_filtered, encoding, chunk (binary)
// The payload: {"version","draw","columns","recordsTotal","recordsFiltered","rows"}
// with every value a JSON string, or null. The version is a hash of the rows,
// so an answer rebuilt after a write that changed nothing keeps its version.
#include <memory>
#include <string>
#include <vector>

namespace aios::userland {

class SqlDatabase;
struct SqlQueryResult;
struct ResultViewStore;

std::shared_ptr<ResultViewStore> sql_result_views_make();
bool sql_is_result_view(const std::string& sql);
SqlQueryResult sql_run_result_view(SqlDatabase& db, const std::string& sql, const std::string& session_db);
// Drops the views whose statement names one of these tables (unqualified).
void sql_result_views_invalidate(ResultViewStore* store, const std::vector<std::string>& tables);
void sql_result_views_invalidate_all(ResultViewStore* store);

}  // namespace aios::userland
