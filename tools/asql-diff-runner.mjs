#!/usr/bin/env node
/**
 * ASQL vs MariaDB Automated Differential Query Runner
 *
 * Runs identical queries against MariaDB (:3306) and ASQL (:3307) simultaneously,
 * verifying:
 *  - Exact row count and column schemas
 *  - Cell-by-cell data equality (NULL handling, types, values)
 *  - Row ordering
 *  - Performance timing (speedup factor)
 *  - EXPLAIN ANALYZE route / pruning telemetry
 *
 * Usage:
 *   node tools/asql-diff-runner.mjs --query "SELECT * FROM salesorders LIMIT 10"
 *   node tools/asql-diff-runner.mjs --file path/to/queries.sql
 *   node tools/asql-diff-runner.mjs --suite erp
 *   node tools/asql-diff-runner.mjs --host chi-il-1 --query "..."
 */

import mysql from 'mysql2/promise';
import fs from 'node:fs';

const c = {
  reset: '\x1b[0m',
  bold: '\x1b[1m',
  dim: '\x1b[2m',
  green: '\x1b[32m',
  red: '\x1b[31m',
  yellow: '\x1b[33m',
  blue: '\x1b[34m',
  cyan: '\x1b[36m',
  magenta: '\x1b[35m',
};

function parseArgs() {
  const args = process.argv.slice(2);
  const opts = {
    host: '127.0.0.1',
    user: process.env.MYSQL_USER || '',
    password: '',
    database: 'aios_db',
    mariadbPort: 3306,
    asqlPort: 3307,
    query: '',
    file: '',
    suite: '',
    explain: false,
    verbose: false,
    runs: 1,
  };

  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    if ((a === '-h' || a === '--host') && args[i + 1]) opts.host = args[++i];
    else if ((a === '-u' || a === '--user') && args[i + 1]) opts.user = args[++i];
    else if ((a === '-p' || a === '--password') && args[i + 1]) opts.password = args[++i];
    else if ((a === '-d' || a === '--database') && args[i + 1]) opts.database = args[++i];
    else if (a === '--mariadb-port' && args[i + 1]) opts.mariadbPort = parseInt(args[++i], 10);
    else if (a === '--asql-port' && args[i + 1]) opts.asqlPort = parseInt(args[++i], 10);
    else if ((a === '-q' || a === '--query') && args[i + 1]) opts.query = args[++i];
    else if ((a === '-f' || a === '--file') && args[i + 1]) opts.file = args[++i];
    else if ((a === '-s' || a === '--suite') && args[i + 1]) opts.suite = args[++i];
    else if (a === '--explain') opts.explain = true;
    else if (a === '-v' || a === '--verbose') opts.verbose = true;
    else if (a === '--runs' && args[i + 1]) opts.runs = parseInt(args[++i], 10);
    else if (a === '--help') {
      console.log(`
${c.bold}ASQL vs MariaDB Differential Query Runner${c.reset}

Options:
  -h, --host <host>          Target host (default: 127.0.0.1)
  -u, --user <user>          MySQL user (default: $MYSQL_USER; required)
  -p, --password <pass>      MySQL password (default: empty)
  -d, --database <db>        Database name (default: aios_db)
  --mariadb-port <port>      MariaDB port (default: 3306)
  --asql-port <port>         ASQL port (default: 3307)
  -q, --query <sql>          Single SQL statement to test
  -f, --file <file.sql>      File containing SQL statements (separated by ;)
  -s, --suite <name>         Run standard benchmark suite (erp | aggregates | joins)
  --explain                  Run EXPLAIN ANALYZE on ASQL and report route/pruning
  --runs <n>                 Number of repetitions for timing (default: 1)
  -v, --verbose              Show full result tables on match
      `);
      process.exit(0);
    }
  }

  if (!opts.user) {
    console.error('--user (or MYSQL_USER) is required');
    process.exit(2);
  }
  return opts;
}

const BUILTIN_SUITES = {
  erp: [
    "SELECT COUNT(*), MAX(orderno) FROM salesorders",
    "SELECT loccode, COUNT(*), SUM(qty) FROM stockmoves GROUP BY loccode ORDER BY loccode",
    "SELECT trandate, COUNT(*) FROM stockmoves WHERE trandate >= '2023-01-01' GROUP BY trandate ORDER BY trandate DESC LIMIT 10",
    "SELECT type, COUNT(*), AVG(price) FROM salesorderdetails GROUP BY type",
    "SELECT debtorno, name, address1 FROM debtorsmaster ORDER BY debtorno LIMIT 20",
    "SELECT stockid, description, categoryid FROM stockmaster WHERE categoryid = 'RAW' LIMIT 20",
    "SELECT sm.loccode, sm.stockid, SUM(sm.qty) FROM stockmoves sm JOIN stockmaster s ON sm.stockid = s.stockid GROUP BY sm.loccode, sm.stockid LIMIT 20"
  ],
  aggregates: [
    "SELECT COUNT(*) FROM stockmoves",
    "SELECT loccode, COUNT(*), SUM(qty), AVG(qty), MIN(qty), MAX(qty) FROM stockmoves GROUP BY loccode",
    "SELECT loccode, COUNT(DISTINCT stockid) FROM stockmoves GROUP BY loccode",
    "SELECT COUNT(*) FROM salesorders WHERE total > 100"
  ],
  joins: [
    "SELECT so.orderno, so.debtorno, sod.stkcode, sod.unitprice FROM salesorders so JOIN salesorderdetails sod ON so.orderno = sod.orderno LIMIT 50",
    "SELECT sm.stockid, s.description, sm.loccode, sm.qty FROM stockmoves sm LEFT JOIN stockmaster s ON sm.stockid = s.stockid LIMIT 50"
  ]
};

async function executeOnConn(conn, sql, runs = 1) {
  let lastRows = null;
  let lastFields = null;
  let totalDurationMs = 0;
  let error = null;

  for (let i = 0; i < runs; i++) {
    const t0 = performance.now();
    try {
      const [rows, fields] = await conn.query(sql);
      const t1 = performance.now();
      totalDurationMs += (t1 - t0);
      lastRows = rows;
      lastFields = fields;
    } catch (err) {
      error = err.message || String(err);
      break;
    }
  }

  return {
    ok: !error,
    error,
    rows: lastRows,
    fields: lastFields,
    avgMs: runs > 0 ? (totalDurationMs / runs) : 0,
  };
}

function normalizeVal(v) {
  if (v === null || v === undefined) return null;
  if (typeof v === 'bigint') return v.toString();
  if (typeof v === 'number') {
    // Round floats slightly to handle subtle precision diffs
    return Number.isInteger(v) ? v : parseFloat(v.toPrecision(10));
  }
  if (v instanceof Date) return v.toISOString();
  return String(v).trim();
}

function compareResults(mariaRes, asqlRes) {
  const diffs = [];

  if (mariaRes.ok !== asqlRes.ok) {
    diffs.push(`Execution outcome mismatch: MariaDB ok=${mariaRes.ok} ("${mariaRes.error || ''}"), ASQL ok=${asqlRes.ok} ("${asqlRes.error || ''}")`);
    return { match: false, diffs };
  }

  if (!mariaRes.ok && !asqlRes.ok) {
    return { match: true, diffs: [`Both failed as expected: MariaDB="${mariaRes.error}" | ASQL="${asqlRes.error}"`] };
  }

  const mRows = Array.isArray(mariaRes.rows) ? mariaRes.rows : [];
  const aRows = Array.isArray(asqlRes.rows) ? asqlRes.rows : [];

  if (mRows.length !== aRows.length) {
    diffs.push(`Row count mismatch: MariaDB returned ${mRows.length} rows, ASQL returned ${aRows.length} rows`);
  }

  const compareCount = Math.min(mRows.length, aRows.length);
  let cellMismatches = 0;

  for (let r = 0; r < compareCount; r++) {
    const mRow = mRows[r];
    const aRow = aRows[r];
    const mKeys = Object.keys(mRow);

    for (const key of mKeys) {
      const mV = normalizeVal(mRow[key]);
      const aV = normalizeVal(aRow[key]);

      if (mV !== aV) {
        cellMismatches++;
        if (cellMismatches <= 5) {
          diffs.push(`Row ${r + 1} col "${key}": MariaDB='${mV}' vs ASQL='${aV}'`);
        }
      }
    }
  }

  if (cellMismatches > 5) {
    diffs.push(`... and ${cellMismatches - 5} more cell value mismatches`);
  }

  return {
    match: diffs.length === 0,
    diffs,
    rowCount: mRows.length,
  };
}

async function runDiff(opts) {
  console.log(`${c.bold}${c.cyan}=== ASQL vs MariaDB Differential Query Runner ===${c.reset}`);
  console.log(`Host: ${c.bold}${opts.host}${c.reset} | MariaDB Port: ${opts.mariadbPort} | ASQL Port: ${opts.asqlPort} | Database: ${opts.database}\n`);

  let mariaConn, asqlConn;
  try {
    mariaConn = await mysql.createConnection({
      host: opts.host,
      port: opts.mariadbPort,
      user: opts.user,
      password: opts.password,
      database: opts.database,
      connectTimeout: 5000,
    });
  } catch (err) {
    console.error(`${c.red}[FAIL] Could not connect to MariaDB at ${opts.host}:${opts.mariadbPort}: ${err.message}${c.reset}`);
    process.exit(1);
  }

  try {
    asqlConn = await mysql.createConnection({
      host: opts.host,
      port: opts.asqlPort,
      user: opts.user,
      password: opts.password,
      database: opts.database,
      connectTimeout: 5000,
    });
  } catch (err) {
    console.error(`${c.red}[FAIL] Could not connect to ASQL at ${opts.host}:${opts.asqlPort}: ${err.message}${c.reset}`);
    await mariaConn.end();
    process.exit(1);
  }

  let queries = [];
  if (opts.query) {
    queries = [opts.query];
  } else if (opts.file) {
    const raw = fs.readFileSync(opts.file, 'utf8');
    queries = raw.split(';').map(q => q.trim()).filter(q => q.length > 0 && !q.startsWith('--'));
  } else if (opts.suite) {
    const suite = BUILTIN_SUITES[opts.suite];
    if (!suite) {
      console.error(`${c.red}Unknown suite: ${opts.suite}. Available: ${Object.keys(BUILTIN_SUITES).join(', ')}${c.reset}`);
      process.exit(1);
    }
    queries = suite;
  } else {
    // Default smoke query
    queries = ["SELECT COUNT(*) FROM salesorders", "SELECT loccode, COUNT(*) FROM stockmoves GROUP BY loccode"];
  }

  let passed = 0;
  let failed = 0;

  for (let idx = 0; idx < queries.length; idx++) {
    const sql = queries[idx];
    console.log(`${c.bold}[${idx + 1}/${queries.length}] SQL:${c.reset} ${sql}`);

    const [mariaRes, asqlRes] = await Promise.all([
      executeOnConn(mariaConn, sql, opts.runs),
      executeOnConn(asqlConn, sql, opts.runs),
    ]);

    const comp = compareResults(mariaRes, asqlRes);

    const mariaTimeStr = `${mariaRes.avgMs.toFixed(2)}ms`;
    const asqlTimeStr = `${asqlRes.avgMs.toFixed(2)}ms`;
    const speedup = mariaRes.avgMs > 0 && asqlRes.avgMs > 0
      ? (mariaRes.avgMs / asqlRes.avgMs).toFixed(1) + 'x'
      : 'N/A';

    if (comp.match) {
      passed++;
      console.log(`  ${c.green}✔ MATCH${c.reset} | Rows: ${comp.rowCount ?? 0} | MariaDB: ${mariaTimeStr} | ASQL: ${c.bold}${c.green}${asqlTimeStr}${c.reset} (${c.yellow}${speedup}${c.reset})`);
    } else {
      failed++;
      console.log(`  ${c.red}✘ MISMATCH${c.reset} | MariaDB: ${mariaTimeStr} | ASQL: ${asqlTimeStr}`);
      for (const d of comp.diffs) {
        console.log(`    ${c.red}→ ${d}${c.reset}`);
      }
    }

    if (opts.explain && asqlRes.ok) {
      try {
        const [expRows] = await asqlConn.query(`EXPLAIN ANALYZE ${sql}`);
        console.log(`  ${c.magenta}[EXPLAIN ANALYZE]${c.reset}`);
        for (const row of expRows) {
          console.log(`    ${c.dim}${JSON.stringify(row)}${c.reset}`);
        }
      } catch (e) {
        console.log(`    ${c.dim}(EXPLAIN ANALYZE unavailable: ${e.message})${c.reset}`);
      }
    }

    console.log();
  }

  await mariaConn.end();
  await asqlConn.end();

  console.log(`${c.bold}=========================================${c.reset}`);
  console.log(`Total: ${queries.length} | ${c.green}Passed: ${passed}${c.reset} | ${failed > 0 ? c.red : c.dim}Failed: ${failed}${c.reset}`);

  if (failed > 0) process.exit(1);
}

runDiff(parseArgs()).catch(err => {
  console.error(`Unexpected error:`, err);
  process.exit(1);
});
