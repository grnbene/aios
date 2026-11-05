// DuckDB's run of the bench_sales tests, in process: duckbench <db file> <bench dir>
// The same statements as bench.php; each time is taken around duckdb_query, which returns the whole answer.
// Prints JSON in bench.php's shape.
#include <duckdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
// This process's CPU time (user + system, all threads) and resident memory: DuckDB runs in it.
static double cpu_ms(void) { struct rusage u; getrusage(RUSAGE_SELF, &u); return u.ru_utime.tv_sec * 1e3 + u.ru_utime.tv_usec / 1e3 + u.ru_stime.tv_sec * 1e3 + u.ru_stime.tv_usec / 1e3; }
static void mem_mb(double* rss, double* hwm) { FILE* f = fopen("/proc/self/status", "r"); char l[256]; double a = 0, b = 0; *rss = *hwm = 0;
  if (!f) return;
  while (fgets(l, sizeof l, f)) { if (sscanf(l, "VmRSS: %lf", &a) == 1) *rss = a / 1024; if (sscanf(l, "VmHWM: %lf", &b) == 1) *hwm = b / 1024; }
  fclose(f); }
static void print_res(double c0, double w0) { double r, h; mem_mb(&r, &h); printf("{\"cpu_ms\":%.1f,\"wall_ms\":%.3f,\"rss_mb\":%.1f,\"hwm_mb\":%.1f}", cpu_ms() - c0, now_ms() - w0, r, h); }
static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
static double median5(double* t) { double c[5]; memcpy(c, t, sizeof c); qsort(c, 5, sizeof(double), cmp); return c[2]; }
static double pct(double* t, int n, int p) { double* c = malloc(n * sizeof(double)); memcpy(c, t, n * sizeof(double)); qsort(c, n, sizeof(double), cmp);
  int i = (int)(p / 100.0 * n); if (i >= n) i = n - 1; double v = c[i]; free(c); return v; }

static duckdb_connection con;
static void run(const char* sql, duckdb_result* keep) {
  duckdb_result r;
  if (duckdb_query(con, sql, &r) == DuckDBError) { fprintf(stderr, "error: %s\nin: %.200s\n", duckdb_result_error(&r), sql); exit(1); }
  if (keep) *keep = r; else duckdb_destroy_result(&r);
}
static void print_answer(duckdb_result* r) {
  idx_t rows = duckdb_row_count(r), cols = duckdb_column_count(r);
  printf("[");
  for (idx_t i = 0; i < rows; i++) {
    printf("%s[", i ? "," : "");
    for (idx_t c = 0; c < cols; c++) { char* v = duckdb_value_varchar(r, c, i); printf("%s\"%s\"", c ? "," : "", v ? v : "NULL"); duckdb_free(v); }
    printf("]");
  }
  printf("]");
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: duckbench <db file> <bench dir>\n"); return 2; }
  duckdb_database dbh;
  remove(argv[1]);
  if (duckdb_open(argv[1], &dbh) == DuckDBError || duckdb_connect(dbh, &con) == DuckDBError) { fprintf(stderr, "open failed\n"); return 1; }
  char path[4096], sql[8192];
  // duckbench <db file> <bench dir> [double]: the amount column is DECIMAL(12,2) unless "double" is given
  const int dbl = argc > 3 && strcmp(argv[3], "double") == 0;
  const char* ddl = dbl ? "(id INT PRIMARY KEY, customer_id INT, category_id INT, amount DOUBLE, trans_date DATE, status VARCHAR(16))"
                        : "(id INT PRIMARY KEY, customer_id INT, category_id INT, amount DECIMAL(12,2), trans_date DATE, status VARCHAR(16))";
  printf("{\"engine\":\"duckdb\",\"amount_type\":\"%s\"", dbl ? "DOUBLE" : "DECIMAL(12,2)");
  { double r, h; mem_mb(&r, &h); printf(",\"res_start\":{\"rss_mb\":%.1f,\"hwm_mb\":%.1f}", r, h); }
  double c0, w0;

  // 1B. the 100 INSERTs of 5,000 rows
  snprintf(sql, sizeof sql, "CREATE TABLE bench_sales %s", ddl); run(sql, NULL);
  snprintf(path, sizeof path, "%s/data/inserts.sql", argv[2]);
  FILE* f = fopen(path, "r"); if (!f) { perror(path); return 1; }
  size_t cap = 1 << 20; char* line = malloc(cap); ssize_t n; double t0 = now_ms(); c0 = cpu_ms(); w0 = t0;
  while ((n = getline(&line, &cap, f)) > 0) { while (n && (line[n - 1] == '\n' || line[n - 1] == ';')) line[--n] = 0; run(line, NULL); }
  printf(",\"wire_insert_ms\":%.3f", now_ms() - t0); printf(",\"res_wire_insert\":"); print_res(c0, w0); fclose(f);
  duckdb_result r; run("SELECT COUNT(*) FROM bench_sales", &r); printf(",\"rows\":%lld", (long long)duckdb_value_int64(&r, 0, 0)); duckdb_destroy_result(&r);

  // 1A. COPY from the CSV
  snprintf(sql, sizeof sql, "CREATE TABLE bench_load %s", ddl); run(sql, NULL);
  snprintf(sql, sizeof sql, "COPY bench_load FROM '%s/data/bench_sales.csv' (FORMAT csv, HEADER false)", argv[2]);
  t0 = now_ms(); c0 = cpu_ms(); w0 = t0; run(sql, NULL); printf(",\"bulk_load_ms\":%.3f", now_ms() - t0); printf(",\"res_bulk_load\":"); print_res(c0, w0);
  run("SELECT COUNT(*) FROM bench_load", &r); printf(",\"bulk_rows\":%lld", (long long)duckdb_value_int64(&r, 0, 0)); duckdb_destroy_result(&r);
  run("DROP TABLE bench_load", NULL);

  // 2. the queries
  const char* names[] = {"full_agg", "group_topn", "range_agg", "pivot", "window"};
  const char* qs[] = {
    "SELECT COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM bench_sales",
    "SELECT category_id, COUNT(*) AS n, SUM(amount) AS total FROM bench_sales GROUP BY category_id ORDER BY total DESC LIMIT 10",
    "SELECT COUNT(*), SUM(amount) FROM bench_sales WHERE trans_date BETWEEN '2024-03-01' AND '2024-08-31' AND amount > 100",
    "SELECT category_id, SUM(CASE WHEN status = 'shipped' THEN amount ELSE 0 END) AS s, SUM(CASE WHEN status = 'pending' THEN amount ELSE 0 END) AS p, SUM(CASE WHEN status = 'returned' THEN amount ELSE 0 END) AS r FROM bench_sales GROUP BY category_id ORDER BY category_id",
    "SELECT COUNT(*), SUM(amount) FROM (SELECT amount, ROW_NUMBER() OVER (PARTITION BY customer_id ORDER BY trans_date DESC, id DESC) AS rn FROM bench_sales) t WHERE rn = 1"};
  printf(",\"query\":{");
  double qcpu[5], qwall[5], qrss[5], qhwm[5]; int qn[5];
  for (int k = 0; k < 5; k++) {
    run(qs[k], NULL);
    double t[5]; duckdb_result last;
    for (int i = 0; i < 5; i++) { double a = now_ms(); run(qs[k], &last); t[i] = now_ms() - a; if (i < 4) duckdb_destroy_result(&last); }
    // the CPU of this query: run for at least a second (and 5 times), as the server-side figures are
    c0 = cpu_ms(); w0 = now_ms(); qn[k] = 0;
    while (qn[k] < 5 || now_ms() - w0 < 1000) { run(qs[k], NULL); qn[k]++; }
    qcpu[k] = cpu_ms() - c0; qwall[k] = now_ms() - w0; mem_mb(&qrss[k], &qhwm[k]);
    double mn = t[0], mx = t[0]; for (int i = 1; i < 5; i++) { if (t[i] < mn) mn = t[i]; if (t[i] > mx) mx = t[i]; }
    printf("%s\"%s\":{\"median_ms\":%.3f,\"min_ms\":%.3f,\"max_ms\":%.3f,\"answer\":", k ? "," : "", names[k], median5(t), mn, mx);
    print_answer(&last); duckdb_destroy_result(&last); printf("}");
  }
  printf("}");
  printf(",\"res_query\":{");
  for (int k = 0; k < 5; k++) printf("%s\"%s\":{\"cpu_ms\":%.1f,\"wall_ms\":%.3f,\"rss_mb\":%.1f,\"hwm_mb\":%.1f,\"n\":%d}", k ? "," : "", names[k], qcpu[k], qwall[k], qrss[k], qhwm[k], qn[k]);
  printf("}");

  // 3. point lookups and 4. updates: the same counts as bench.php (its ids are PHP's mt_rand; these are random() with
  // a fixed seed - the same distribution over 1..500000, not the same ids)
  double* t = malloc(10000 * sizeof(double)); srandom(7); t0 = now_ms(); c0 = cpu_ms(); w0 = t0;
  for (int i = 0; i < 10000; i++) { snprintf(sql, sizeof sql, "SELECT id, customer_id, amount, status FROM bench_sales WHERE id = %ld", 1 + random() % 500000);
    double a = now_ms(); run(sql, NULL); t[i] = now_ms() - a; }
  double tot = now_ms() - t0;
  printf(",\"res_point\":"); print_res(c0, w0);
  printf(",\"point\":{\"n\":10000,\"p50_ms\":%.4f,\"p99_ms\":%.4f,\"qps\":%.1f}", pct(t, 10000, 50), pct(t, 10000, 99), 10000 / (tot / 1000));
  srandom(11); t0 = now_ms(); c0 = cpu_ms(); w0 = t0;
  for (int i = 0; i < 2000; i++) { snprintf(sql, sizeof sql, "UPDATE bench_sales SET amount = amount + 0.01 WHERE id = %ld", 1 + random() % 500000);
    double a = now_ms(); run(sql, NULL); t[i] = now_ms() - a; }
  tot = now_ms() - t0;
  printf(",\"res_update\":"); print_res(c0, w0);
  printf(",\"update\":{\"n\":2000,\"p50_ms\":%.4f,\"p99_ms\":%.4f,\"tps\":%.1f}", pct(t, 2000, 50), pct(t, 2000, 99), 2000 / (tot / 1000));
  run("CHECKPOINT", NULL);
  printf("}\n");
  duckdb_disconnect(&con); duckdb_close(&dbh);
  return 0;
}
