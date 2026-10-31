// The physical plan a statement ran with (docs/asql-vectorized-plan.md, stage 2a): the decisions the select
// engine makes inline - which source is the base, how each join step probes, how the groups are made, what form
// the answer leaves in - written down as it makes them, with each step's rows and time. EXPLAIN prints it.
// Recording is on only while a PhysicalPlan is the thread's sink (PlanScope); otherwise plan_add is a null check.
// A nested statement (a CTE, a derived table) records into its own plan, kept as the step's `sub`.
struct PlanStep {
  std::string op;        // source, base, join, left join, group by, sort, emit, cte, derived, single-table, ...
  std::string name;      // the alias or table it concerns
  std::string method;    // how it ran: "index probe, batched", "integer lanes", "merged as a view", ...
  std::string detail;    // keys, conditions, counts worth showing
  size_t rows = SIZE_MAX;   // rows out, where known
  uint64_t ns = 0;          // its time, where measured
  std::vector<PlanStep> sub;
};
struct PhysicalPlan {
  std::vector<PlanStep> steps;
};
thread_local PhysicalPlan* g_plan = nullptr;
inline bool plan_on() { return g_plan != nullptr; }
inline void plan_add(PlanStep s) { if (g_plan) g_plan->steps.push_back(std::move(s)); }
inline PlanStep plan_step(const char* op, std::string name = {}, std::string method = {}, size_t rows = SIZE_MAX, uint64_t ns = 0) {
  PlanStep s;
  s.op = op; s.name = std::move(name); s.method = std::move(method); s.rows = rows; s.ns = ns;
  return s;
}
// The thread records into `p` while this lives (nullptr: records nothing, as a nested statement under no EXPLAIN).
struct PlanScope {
  PhysicalPlan* prev;
  explicit PlanScope(PhysicalPlan* p) : prev(g_plan) { g_plan = p; }
  ~PlanScope() { g_plan = prev; }
};
// One line a step, nested steps indented under theirs: "-> join nc [index probe, batched]  rows=472  0.12 ms".
inline void plan_print(const std::vector<PlanStep>& steps, std::vector<std::string>* out, int depth = 0) {
  for (const PlanStep& s : steps) {
    std::string line(depth * 4, ' ');
    line += "-> " + s.op;
    if (!s.name.empty()) line += " " + s.name;
    if (!s.method.empty()) line += " [" + s.method + "]";
    if (!s.detail.empty()) line += " (" + s.detail + ")";
    if (s.rows != SIZE_MAX) line += "  rows=" + std::to_string(s.rows);
    if (s.ns) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "  %.2f ms", s.ns / 1e6);
      line += buf;
    }
    out->push_back(std::move(line));
    plan_print(s.sub, out, depth + 1);
  }
}
