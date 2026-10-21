// AIOS — Multi-Dialect Write-Behind Pipeline Driver.
// Prints what the write-behind would send a T-SQL and a PostgreSQL downstream
// for a few statements, and how downstream URIs are read. Nothing is sent:
// aios-sql-dialect-drive --pg=... runs the translations on a real server.
#include "aios/kernel/kernel.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/sql_dialect.hpp"
#include "aios/userland/sql_wire.hpp"
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace aios;
using namespace aios::userland;

int main() {
  printf("================================================================================\n");
  printf("  AIOS UNIVERSAL RELATIONAL ACCELERATOR: MULTI-DIALECT GATEWAY DRIVER\n");
  printf("================================================================================\n\n");

  KernelConfig kcfg;
  kcfg.root = "/tmp/aios-multidialect-test";
  auto k_res = Kernel::boot(kcfg);
  if (!k_res.ok()) {
    printf("[-] Failed to boot AIOS Kernel: %s\n", k_res.error().message.c_str());
    return 1;
  }
  Kernel& kernel = **k_res;
  SqlDatabase db(kernel);
  auto init_r = db.init("facets_claims_db");
  if (!init_r.ok()) {
    printf("[-] Failed to init SqlDatabase: %s\n", init_r.error().message.c_str());
    return 1;
  }

  printf("[1] Initialized In-Memory Multi-Tenant ASQL Engine (Database: facets_claims_db)\n");

  // Sample claims mutations recorded into the Write-Behind WAL
  std::vector<std::string> mutations = {
    "CREATE TABLE `claims` (`claim_id` INT AUTO_INCREMENT PRIMARY KEY, `patient_id` INT, `paid_amount` DECIMAL(10,2), `adjudicated` TINYINT(1) DEFAULT 0, `adjudicated_at` DATETIME) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;",
    "INSERT INTO `claims` (`claim_id`, `patient_id`, `paid_amount`, `adjudicated`, `adjudicated_at`) VALUES (1001, 550, 425.50, TRUE, NOW()) ON DUPLICATE KEY UPDATE `paid_amount` = VALUES(`paid_amount`), `adjudicated` = VALUES(`adjudicated`);",
    "SELECT `claim_id`, `paid_amount`, IFNULL(`notes`, 'CLEARED') FROM `claims` WHERE `adjudicated_at` >= DATE_SUB(NOW(), INTERVAL 7 DAY) LIMIT 100 OFFSET 0;"
  };

  printf("[2] Translations (printed, not executed):\n\n");

  for (size_t i = 0; i < mutations.size(); ++i) {
    printf("--- Mutation %zu ---\n", i + 1);
    printf("  [ASQL Normalized Mutation]:\n    %s\n\n", mutations[i].c_str());

    auto t0 = std::chrono::high_resolution_clock::now();
    std::string tsql = SqlDialectTranslator::to_mssql(mutations[i]);   // "-- untranslatable: ..." when it cannot be
    auto t1 = std::chrono::high_resolution_clock::now();
    double tsql_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    printf("  [Drained to MS SQL Server / FACETS (TDS 7.4+, %.2f µs)]:\n    %s\n\n", tsql_us, tsql.c_str());

    auto p0 = std::chrono::high_resolution_clock::now();
    std::string pgsql = SqlDialectTranslator::to_postgres(mutations[i]);
    auto p1 = std::chrono::high_resolution_clock::now();
    double pgsql_us = std::chrono::duration<double, std::micro>(p1 - p0).count();

    printf("  [Drained to PostgreSQL (PGWire v3, %.2f µs)]:\n    %s\n\n", pgsql_us, pgsql.c_str());
  }

  printf("[3] Testing URI Configuration Resolution for Enterprise Downstreams:\n");
  const char* uris[] = {
    "tds://sa:Secret%40Pass@192.168.1.50:1433/FACETS?encrypt=true",
    "postgres://postgres:ClaimsDbPass@[::1]:5432/claims_warehouse?sslmode=require&sslrootcert=/etc/ssl/db-ca.pem",
    "mysql://root:rootpass@127.0.0.1:3306/claims_oltp"
  };

  for (const char* uri_str : uris) {
    auto p_res = DbConnectionProfile::from_uri(uri_str);
    if (!p_res.ok()) {
      printf("  [-] Failed to parse %s: %s\n", uri_str, p_res.error().message.c_str());
      return 1;
    }
    const auto& p = *p_res;
    printf("  ✓ URI: %s\n", uri_str);
    printf("      Dialect : %s\n", sql_dialect_name(p.dialect));
    printf("      Endpoint: %s:%u\n", p.host.c_str(), p.port);
    printf("      User    : %s\n", p.user.c_str());
    printf("      Database: %s\n", p.database.c_str());
    printf("      TLS     : %s%s\n\n", p.ssl_mode.empty() ? "prefer (default)" : p.ssl_mode.c_str(), p.ssl_ca.empty() ? "" : (" ca=" + p.ssl_ca).c_str());
  }

  printf("================================================================================\n");
  printf("  Translations printed; URIs parsed. (Execution against real servers: aios-sql-dialect-drive.)\n");
  printf("================================================================================\n");

  return 0;
}
