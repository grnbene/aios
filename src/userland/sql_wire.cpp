// AIOS — SqlWireSession factory implementation.
#include "aios/userland/sql_wire.hpp"
#include "aios/userland/mysql_wire.hpp"
#include "aios/userland/pg_wire.hpp"
#include "aios/userland/tds_wire.hpp"

namespace aios::userland {

std::shared_ptr<SqlWireSession> create_sql_wire_session(
    SqlDialect dialect, const std::string& host, uint16_t port) {
  switch (dialect) {
    case SqlDialect::MariaDB:
      return std::make_shared<MariaWireSession>(host, port ? port : 3306);
    case SqlDialect::Postgres:
      return std::make_shared<PgWireSession>(host, port ? port : 5432);
    case SqlDialect::MsSql:
      return std::make_shared<TdsWireSession>(host, port ? port : 1433);
  }
  return std::make_shared<MariaWireSession>(host, port ? port : 3306);
}

}  // namespace aios::userland
