// The three ERP reports on the tenth pass's DuckDB copy of summitholding: duckerp <db> <dir>; one warm-up, 5 timed runs each.
#include <duckdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }
int main(int argc, char** argv) {
  duckdb_database dbh; duckdb_connection con; duckdb_config cfg;
  duckdb_create_config(&cfg); duckdb_set_config(cfg, "access_mode", "READ_ONLY");
  char* err = NULL;
  if (duckdb_open_ext(argv[1], &dbh, cfg, &err) == DuckDBError || duckdb_connect(dbh, &con) == DuckDBError) { fprintf(stderr, "open: %s\n", err ? err : "?"); return 1; }
  const char* names[] = {"thisoverlast", "nopurchase", "brandbylocation"};
  printf("{");
  for (int i = 1; i <= 3; i++) {
    char path[4096]; snprintf(path, sizeof path, "%s/erp/duck%d.sql", argv[2], i);
    FILE* f = fopen(path, "r"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char* sql = calloc(n + 1, 1); fread(sql, 1, n, f); fclose(f);
    duckdb_result r;
    if (duckdb_query(con, sql, &r) == DuckDBError) { fprintf(stderr, "%s\n", duckdb_result_error(&r)); return 1; }
    duckdb_destroy_result(&r);
    double t[5];
    for (int k = 0; k < 5; k++) { double a = now_ms(); duckdb_query(con, sql, &r); t[k] = now_ms() - a; if (k < 4) duckdb_destroy_result(&r); }
    qsort(t, 5, sizeof(double), cmp);
    idx_t rows = duckdb_row_count(&r), cols = duckdb_column_count(&r);
    printf("%s\"%s\":{\"median_ms\":%.3f,\"min_ms\":%.3f,\"max_ms\":%.3f,\"rows\":%llu,\"answer\":[", i > 1 ? "," : "", names[i - 1], t[2], t[0], t[4], (unsigned long long)rows);
    for (idx_t y = 0; y < rows; y++) {
      printf("%s[", y ? "," : "");
      for (idx_t c = 0; c < cols; c++) { char* v = duckdb_value_varchar(&r, c, y); printf("%s\"", c ? "," : "");
        for (char* p = v; p && *p; p++) { if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); } else if ((unsigned char)*p < 0x20) printf("\\u%04x", (unsigned char)*p); else putchar(*p); } printf("\""); duckdb_free(v); }
      printf("]");
    }
    printf("]}"); duckdb_destroy_result(&r); free(sql);
  }
  printf("}\n");
  return 0;
}
