#include "aios/storage/aql.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/core/strbuf.hpp"
#include "aios/hal/host.hpp"
#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <cctype>

namespace aios::storage {

std::vector<float> text_embedding(const std::string& text) {
  std::vector<float> v(64, 0.f);
  std::string t;
  t.reserve(text.size() + 4);
  for (char c : text) t.push_back((c >= 'A' && c <= 'Z') ? (char)(c + 32) : c);
  t = "  " + t + "  ";
  for (size_t i = 0; i + 3 <= t.size(); ++i) {
    uint64_t h = 1469598103934665603ull;
    for (size_t j = 0; j < 3; ++j) { h ^= (uint8_t)t[i + j]; h *= 1099511628211ull; }
    const size_t dim = h % 64;
    const float sign = ((h >> 7) & 1) ? 1.f : -1.f;
    v[dim] += sign;
  }
  double n = 0;
  for (float f : v) n += double(f) * f;
  n = std::sqrt(n);
  if (n > 0) for (auto& f : v) f = (float)(f / n);
  return v;
}

namespace {

enum class TokKind { Ident, Str, Num, Punct };

struct Token {
  TokKind kind = TokKind::Ident;
  std::string text;
  std::string upper;
};

std::string to_upper(const std::string& s) {
  std::string out = s;
  for (auto& c : out) c = (char)std::toupper((unsigned char)c);
  return out;
}

bool tokenize(const std::string& input, std::vector<Token>& tokens, std::string& err) {
  size_t i = 0;
  while (i < input.size()) {
    char c = input[i];
    if (std::isspace((unsigned char)c)) { i++; continue; }
    if (c == '-' && i + 1 < input.size() && input[i + 1] == '-') {
      while (i < input.size() && input[i] != '\n') i++;
      continue;
    }
    if (c == '#') {
      while (i < input.size() && input[i] != '\n') i++;
      continue;
    }
    if (c == '\'' || c == '"') {
      char quote = c;
      std::string body;
      size_t j = i + 1;
      bool closed = false;
      while (j < input.size()) {
        if (input[j] == '\\' && j + 1 < input.size()) { body += input[j + 1]; j += 2; continue; }
        if (input[j] == quote) { closed = true; j++; break; }
        body += input[j++];
      }
      if (!closed) { err = "unterminated string literal"; return false; }
      tokens.push_back({TokKind::Str, body, to_upper(body)});
      i = j;
      continue;
    }
    // `<` and `>` used to fall off the end of this chain and be skipped
    // silently, so `WHERE meta.price > 100` parsed as field=meta.price,
    // op=100, value=<whatever came next> and quietly matched nothing.
    if (c == '<' || c == '>') {
      if (i + 1 < input.size() && input[i + 1] == '=') {
        std::string s = std::string(1, c) + "=";
        tokens.push_back({TokKind::Punct, s, s});
        i += 2;
        continue;
      }
      if (c == '<' && i + 1 < input.size() && input[i + 1] == '>') {
        tokens.push_back({TokKind::Punct, "!=", "!="});
        i += 2;
        continue;
      }
      std::string s(1, c);
      tokens.push_back({TokKind::Punct, s, s});
      i++;
      continue;
    }
    if (c == '=' || c == '!' || c == ',' || c == '(' || c == ')' || c == '[' || c == ']' || c == '*' || c == ':' || c == ';') {
      if (c == '!' && i + 1 < input.size() && input[i + 1] == '=') {
        tokens.push_back({TokKind::Punct, "!=", "!="});
        i += 2;
        continue;
      }
      std::string s(1, c);
      tokens.push_back({TokKind::Punct, s, s});
      i++;
      continue;
    }
    if (std::isdigit((unsigned char)c) || (c == '-' && i + 1 < input.size() && std::isdigit((unsigned char)input[i + 1]))) {
      // Take the whole word, then decide: a number only when ALL of it is one
      // ([-]digits[.digits]). An object id is hex and often starts with a
      // digit; reading just the leading digits split `92fd04…` into the number
      // 92 and the identifier `fd04…`, so RELATED TO / HISTORY OF matched
      // nothing and CHECKPOINT(<id>, n) took the digits for a timestamp.
      size_t j = i + 1;
      while (j < input.size() && (std::isalnum((unsigned char)input[j]) || input[j] == '_' || input[j] == '.' || input[j] == ':' || input[j] == '-')) j++;
      std::string val = input.substr(i, j - i);
      size_t k = val[0] == '-' ? 1 : 0, digits = 0, dots = 0;
      bool numeric = true;
      for (; k < val.size(); ++k) {
        if (std::isdigit((unsigned char)val[k])) digits++;
        else if (val[k] == '.' && dots == 0) dots++;
        else { numeric = false; break; }
      }
      numeric = numeric && digits > 0 && val.back() != '.';
      tokens.push_back({numeric ? TokKind::Num : TokKind::Ident, val, numeric ? val : to_upper(val)});
      i = j;
      continue;
    }
    if (std::isalpha((unsigned char)c) || c == '_' || c == '.' || c == ':') {
      size_t j = i;
      while (j < input.size() && (std::isalnum((unsigned char)input[j]) || input[j] == '_' || input[j] == '.' || input[j] == ':' || input[j] == '-')) j++;
      std::string val = input.substr(i, j - i);
      tokens.push_back({TokKind::Ident, val, to_upper(val)});
      i = j;
      continue;
    }
    i++;
  }
  return true;
}

ObjectID parse_object_id(const std::string& text) {
  std::string s = text;
  if (s.rfind("obj:", 0) == 0) s = s.substr(4);
  Id128 raw;
  if (Id128::parse(s, raw)) return ObjectID{raw};
  return ObjectID{};
}

uint32_t parse_kind_value(const std::string& text) {
  if (text == "derived_from" || text == "DERIVED_FROM") return 0xD0;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    return (uint32_t)aios::to_u64(text.c_str(), nullptr, 16);
  }
  if (!text.empty() && std::isdigit((unsigned char)text[0])) {
    return (uint32_t)aios::to_u64(text.c_str());
  }
  uint32_t h = 5381;
  for (char c : text) h = ((h << 5) + h) + (uint8_t)c;
  return h;
}

}  // namespace

Result<AqlQuery> parse_aql(const std::string& query_str) {
  std::vector<Token> tokens;
  std::string terr;
  if (!tokenize(query_str, tokens, terr)) {
    return Result<AqlQuery>(Status::Invalid, "AQL tokenizer error: " + terr);
  }
  if (tokens.empty()) return Result<AqlQuery>(Status::Invalid, "empty AQL query");

  AqlQuery q;
  size_t idx = 0;

  auto peek = [&](size_t offset = 0) -> const Token* {
    return (idx + offset < tokens.size()) ? &tokens[idx + offset] : nullptr;
  };
  auto consume = [&]() -> const Token* {
    return (idx < tokens.size()) ? &tokens[idx++] : nullptr;
  };

  const Token* first = consume();
  if (first->upper == "SELECT") {
    q.verb = AqlVerb::Select;
    // Parse SELECT projection
    while (idx < tokens.size()) {
      const Token* t = peek();
      if (!t) break;
      if (t->upper == "FROM" || t->upper == "WHERE" || t->upper == "NEAR" || t->upper == "RELATED" ||
          t->upper == "HISTORY" || t->upper == "AS" || t->upper == "ORDER" || t->upper == "LIMIT") break;
      if (t->text == "*") { q.select_all = true; consume(); }
      else if (t->upper == "COUNT") {
        // COUNT(*) / COUNT() — the tally of what matched, not the matches.
        consume();
        if (peek() && peek()->text == "(") {
          consume();
          while (peek() && peek()->text != ")") consume();
          if (peek() && peek()->text == ")") consume();
        }
        q.count_only = true;
        q.select_all = false;
      }
      else if (t->kind == TokKind::Ident) {
        if (t->upper != "OBJECTS") q.select_fields.push_back(t->text);
        consume();
      } else {
        consume();
      }
    }
    if (peek() && peek()->upper == "FROM") {
      consume(); // consume FROM
      if (peek() && peek()->upper == "OBJECTS") consume();
    }
  } else if (first->upper == "PUT" || first->upper == "INSERT") {
    q.verb = AqlVerb::Put;
    if (peek() && peek()->upper == "INTO") { consume(); if (peek() && peek()->upper == "OBJECTS") consume(); }
    while (idx < tokens.size()) {
      const Token* k = consume();
      if (!k) break;
      if (peek() && peek()->text == "=") {
        consume(); // =
        const Token* v = consume();
        if (v) {
          std::string key = k->text, val = v->text;
          if (key == "derived" || key == "derived_from") q.put_object.derived_from = parse_object_id(val);
          else if (key == "payload") q.put_object.payload = ByteBuffer(val.begin(), val.end());
          else q.put_object.metadata.emplace_back(key, val);
        }
      } else {
        q.put_object.payload = ByteBuffer(k->text.begin(), k->text.end());
        q.put_object.embedding = text_embedding(k->text);
      }
    }
    return q;
  } else {
    return Result<AqlQuery>(Status::Invalid, "unsupported AQL verb: " + first->text);
  }

  // Parse Clauses: WHERE, NEAR, RELATED TO, HISTORY OF, AS OF, ORDER BY, LIMIT
  while (idx < tokens.size()) {
    const Token* t = consume();
    if (!t) break;

    if (t->upper == "WHERE") {
      std::vector<AqlPredicate> group;
      bool negate_next = false;
      while (idx < tokens.size()) {
        const Token* field_tok = peek();
        if (!field_tok) break;
        if (field_tok->upper == "NEAR" || field_tok->upper == "RELATED" || field_tok->upper == "HISTORY" ||
            field_tok->upper == "AS" || field_tok->upper == "ORDER" || field_tok->upper == "LIMIT") break;
        if (field_tok->upper == "AND") { consume(); continue; }
        if (field_tok->upper == "NOT") { consume(); negate_next = true; continue; }
        if (field_tok->upper == "OR") {
          consume();
          // Everything collected so far is one alternative; what follows is
          // the next one.
          q.predicate_groups.push_back(group);
          group.clear();
          continue;
        }

        const Token* field = consume();
        const Token* op = consume();
        const Token* val = consume();
        if (!field || !op || !val) {
          return Result<AqlQuery>(Status::Invalid,
              "incomplete predicate in WHERE near '" + (field ? field->text : std::string("end of query")) +
              "' — expected <field> <operator> <value>");
        }
        const std::string& o = op->upper;
        if (o != "=" && o != "==" && o != "!=" && o != "<" && o != "<=" && o != ">" && o != ">=" &&
            o != "LIKE" && o != "CONTAINS") {
          return Result<AqlQuery>(Status::Invalid,
              "unknown operator '" + op->text + "' on field '" + field->text +
              "' — expected one of = != < <= > >= LIKE CONTAINS");
        }
        AqlPredicate p;
        p.field = field->text;
        p.op = o;
        p.value = val->text;
        p.negated = negate_next;
        negate_next = false;
        group.push_back(p);
      }
      q.predicate_groups.push_back(group);
      // One group is a plain AND-list; keep it in `predicates` so anything
      // reading the simple field sees the same query.
      if (q.predicate_groups.size() == 1) q.predicates = q.predicate_groups[0];
    } else if (t->upper == "NEAR") {
      q.has_near = true;
      const Token* arg = peek();
      if (arg && (arg->upper == "EMBEDDING_OF" || arg->text == "embedding_of")) {
        consume(); // embedding_of
        if (peek() && peek()->text == "(") consume();
        const Token* str_arg = consume();
        if (str_arg) {
          q.near_text = str_arg->text;
          q.near_embedding = text_embedding(str_arg->text);
        }
        if (peek() && peek()->text == ")") consume();
      } else if (arg && arg->text == "[") {
        consume(); // [
        std::vector<float> vec;
        while (idx < tokens.size()) {
          const Token* elem = consume();
          if (!elem || elem->text == "]") break;
          if (elem->kind == TokKind::Num) vec.push_back((float)aios::to_f64(elem->text.c_str()));
        }
        q.near_embedding = vec;
      } else if (arg) {
        const Token* str_arg = consume();
        q.near_text = str_arg->text;
        q.near_embedding = text_embedding(str_arg->text);
      }
    } else if (t->upper == "RELATED") {
      if (peek() && peek()->upper == "TO") consume();
      q.has_related = true;
      const Token* id_tok = consume();
      if (id_tok) q.related_id = parse_object_id(id_tok->text);
      if (q.related_id.is_null())
        return Result<AqlQuery>(Status::Invalid, "RELATED TO needs a full object id (32 hex digits), got '" + (id_tok ? id_tok->text : std::string("end of query")) + "'");
      if (peek() && peek()->upper == "KIND") {
        consume(); // KIND
        const Token* kind_tok = consume();
        if (kind_tok) {
          q.has_related_kind = true;
          q.related_kind_str = kind_tok->text;
          q.related_kind = parse_kind_value(kind_tok->text);
        }
      }
    } else if (t->upper == "HISTORY") {
      if (peek() && peek()->upper == "OF") consume();
      q.has_history = true;
      const Token* id_tok = consume();
      if (id_tok) q.history_id = parse_object_id(id_tok->text);
      if (q.history_id.is_null())
        return Result<AqlQuery>(Status::Invalid, "HISTORY OF needs a full object id (32 hex digits), got '" + (id_tok ? id_tok->text : std::string("end of query")) + "'");
    } else if (t->upper == "AS") {
      if (peek() && peek()->upper == "OF") consume();
      q.has_as_of = true;
      const Token* ckpt = peek();
      if (!ckpt) return Result<AqlQuery>(Status::Invalid, "AS OF needs a timestamp, TIMESTAMP(<ns>), CHECKPOINT(<entity>, <n>) or a checkpoint path");
      if (ckpt && (ckpt->upper == "CHECKPOINT" || ckpt->text == "checkpoint" || ckpt->upper == "TIMESTAMP" || ckpt->text == "timestamp")) {
        consume();
        if (peek() && peek()->text == "(") consume();
        const Token* arg1 = consume();
        if (!arg1 || arg1->text == ")") return Result<AqlQuery>(Status::Invalid, "AS OF " + ckpt->text + "(...) is missing its argument");
        if (arg1) {
          if (peek() && peek()->text == ",") {
            consume(); // ,
            const Token* arg2 = consume();
            q.as_of_checkpoint_node = arg1->text;
            if (!arg2 || arg2->kind != TokKind::Num)
              return Result<AqlQuery>(Status::Invalid, "AS OF CHECKPOINT(<entity>, <n>): <n> must be a checkpoint number");
            q.as_of_checkpoint_seq = (uint64_t)aios::to_u64(arg2->text.c_str());
          } else {
            q.as_of_path = arg1->text;
            if (arg1->kind == TokKind::Num) q.as_of_timestamp_ns = (uint64_t)aios::to_u64(arg1->text.c_str());
          }
        }
        if (peek() && peek()->text == ")") consume();
      } else if (ckpt) {
        const Token* arg = consume();
        q.as_of_path = arg->text;
        if (arg->kind == TokKind::Num) q.as_of_timestamp_ns = (uint64_t)aios::to_u64(arg->text.c_str());
      }
    } else if (t->upper == "ORDER") {
      if (peek() && peek()->upper == "BY") consume();
      const Token* col = consume();
      if (col) { q.order_by = col->text; q.order_explicit = true; }
      if (peek() && (peek()->upper == "ASC" || peek()->upper == "DESC")) {
        q.order_desc = (consume()->upper == "DESC");
      }
    } else if (t->upper == "LIMIT") {
      const Token* lim = consume();
      if (lim) q.limit = (size_t)aios::to_u64(lim->text.c_str());
    }
  }

  return q;
}

namespace {

// Whether a string is a number all the way through, so ordering comparisons
// can be numeric where that is what the data means. "12" < "9" is true as
// text and false as quantity, and an ERP field held as text means quantity.
bool numeric_of(const std::string& s, double* out) {
  if (s.empty()) return false;
  const char* end = nullptr;
  double v = aios::to_f64(s.c_str(), &end);
  if (end == s.c_str()) return false;
  while (*end && std::isspace((unsigned char)*end)) end++;
  if (*end != '\0') return false;
  *out = v;
  return true;
}

// -1, 0, 1 — numeric when both sides are numbers, lexicographic otherwise.
int compare_values(const std::string& a, const std::string& b) {
  double na = 0, nb = 0;
  if (numeric_of(a, &na) && numeric_of(b, &nb)) {
    if (na < nb) return -1;
    if (na > nb) return 1;
    return 0;
  }
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

// The value a field resolves to for filtering and for ordering — one place, so
// ORDER BY and WHERE cannot disagree about what `meta.price` means.
std::string field_value(const StoredObject& obj, const std::string& field) {
  if (field.rfind("meta.", 0) == 0) return obj.meta(field.substr(5));
  if (field == "id") return obj.id.str();
  if (field == "derived_from") return obj.derived_from.str();
  if (field == "provenance") return obj.provenance.str();
  if (field == "source_state") return obj.source_state.str();
  if (field == "timestamp" || field == "timestamp_ns") return std::to_string(obj.timestamp_ns);
  return obj.meta(field);
}

bool evaluate_predicate(const StoredObject& obj, const AqlPredicate& p) {
  std::string actual;
  if (p.field.rfind("meta.", 0) == 0) {
    actual = obj.meta(p.field.substr(5));
  } else if (p.field == "id") {
    actual = obj.id.str();
  } else if (p.field == "derived_from") {
    actual = obj.derived_from.str();
  } else if (p.field == "provenance") {
    actual = obj.provenance.str();
  } else if (p.field == "source_state") {
    actual = obj.source_state.str();
  } else if (p.field == "timestamp") {
    actual = std::to_string(obj.timestamp_ns);
  } else {
    actual = obj.meta(p.field);
  }

  if (p.op == "=" || p.op == "==") return actual == p.value;
  if (p.op == "!=") return actual != p.value;
  if (p.op == "LIKE" || p.op == "CONTAINS") {
    return actual.find(p.value) != std::string::npos;
  }
  if (p.op == "<") return compare_values(actual, p.value) < 0;
  if (p.op == "<=") return compare_values(actual, p.value) <= 0;
  if (p.op == ">") return compare_values(actual, p.value) > 0;
  if (p.op == ">=") return compare_values(actual, p.value) >= 0;
  return actual == p.value;
}

// The bound an AS OF names, or 0 when it names nothing this machine can find
// and verify — the caller must treat 0 as an error, never as "no bound".
uint64_t resolve_checkpoint_timestamp(hal::Host* host, const std::string& root_dir, const AqlQuery& q, std::string* tried = nullptr) {
  if (q.as_of_timestamp_ns > 0) return q.as_of_timestamp_ns;

  std::vector<std::string> try_paths;
  if (!q.as_of_path.empty()) try_paths.push_back(q.as_of_path);
  if (!q.as_of_checkpoint_node.empty()) {
    std::string base = (root_dir.empty() ? "." : root_dir) + "/checkpoints/" + q.as_of_checkpoint_node;
    if (q.as_of_checkpoint_seq > 0) {
      try_paths.push_back(base + "/" + std::to_string(q.as_of_checkpoint_seq) + ".aoc");
    }
    try_paths.push_back(base + ".aoc");
  }

  if (tried) for (const auto& path : try_paths) *tried += (tried->empty() ? "" : ", ") + path;
  for (const auto& path : try_paths) {
    if (!host) continue;
    auto data = host->read_file(path);
    if (!data.ok() || data->size() < 24) continue;
    Reader hr(*data);
    uint32_t magic = hr.u32(), version = hr.u32();
    (void)version;
    uint64_t len = hr.u64(), sum = hr.u64();
    if (!hr.ok() || magic != 0x434B4F41u || len > hr.remaining()) continue;
    ByteSpan body = hr.raw(len);
    if (fnv1a(body) != sum) continue;
    Reader r(body);
    r.id(); // origin kernel
    uint64_t timestamp_ns = r.u64();
    return timestamp_ns;
  }

  return 0;   // nothing found: an error for the caller, not an unbounded query
}

// How many objects a result's human-readable listing renders before it says
// how many more there are.
constexpr size_t kFormatCap = 50;

}  // namespace

AqlQueryResult execute_aql(const ObjectStore& store, const AqlQuery& query, hal::Host* host, const std::string& root_dir) {
  AqlQueryResult res;

  if (query.verb == AqlVerb::Put) {
    ObjectStore& store_nonconst = const_cast<ObjectStore&>(store);
    auto put_res = store_nonconst.put(query.put_object);
    if (!put_res.ok()) {
      res.success = false;
      res.error = put_res.error().str();
      return res;
    }
    auto get_res = store.get(*put_res);
    if (get_res.ok()) res.objects.push_back(*get_res);
    res.count = res.objects.size();
    res.formatted = "Inserted object " + put_res->str();
    return res;
  }

  // The WHERE clause in disjunctive normal form: match = ANY group matches in
  // full. A query built in C++ sets only `predicates`, which is one group.
  std::vector<std::vector<AqlPredicate>> groups = query.predicate_groups;
  if (groups.empty()) groups.push_back(query.predicates);

  const bool as_of_none = !query.has_as_of;
  // When the answer is going to come from walking an ordering, enumerating
  // every id first is pure waste: store.list() reads every sealed block to
  // produce a list the ordered walk does not need.
  const bool ordered_walk_possible =
      query.limit > 0 && query.order_by.rfind("meta.", 0) == 0 &&
      !query.has_near && !query.has_related && !query.has_history &&
      store.numeric_ordered(query.order_by.substr(5));

  // 1. Initial Candidates
  //
  // Seeding matters more than filtering: every candidate costs a store.get()
  // and a predicate pass, so the plan is to start from the smallest set that
  // provably contains every answer. An indexed equality gives us that; with
  // several to choose from, the rarest value wins.
  std::vector<ObjectID> candidates;
  if (query.has_near) {
    size_t k_want = query.limit > 0 ? query.limit * 4 + 10 : 100;
    auto hits = store.find_similar(query.near_embedding, k_want);
    for (auto& h : hits) candidates.push_back(h.id);
  } else if (query.has_related) {
    auto rels = store.find_related(query.related_id);
    // An object related twice (a rel= edge and its derived_from edge) is one
    // answer, not two.
    std::unordered_set<ObjectID, TypedIdHash<9>> seen;
    for (auto& r : rels) {
      if (!query.has_related_kind || r.kind == query.related_kind) {
        if (seen.insert(r.target).second) candidates.push_back(r.target);
      }
    }
  } else if (query.has_history) {
    candidates = store.find_history(query.history_id);
  } else if (groups.size() == 1) {
    // Composite Predicate Pushdown: collect all indexable candidate sets (equalities & ranges)
    std::vector<std::vector<ObjectID>> index_sets;
    for (const auto& p : groups[0]) {
      if (p.negated) continue;
      if ((p.op == "=" || p.op == "==") && p.field.rfind("meta.", 0) == 0) {
        const std::string mk = p.field.substr(5);
        if (store.meta_indexed(mk)) {
          index_sets.push_back(store.find_meta_exact(mk, p.value));
        }
      } else if ((p.op == "<" || p.op == "<=" || p.op == ">" || p.op == ">=") && p.field.rfind("meta.", 0) == 0) {
        const std::string mk = p.field.substr(5);
        double bound = 0;
        if (numeric_of(p.value, &bound)) {
          std::vector<ObjectID> ranged;
          if (store.range_meta(mk, p.op, bound, ranged)) {
            index_sets.push_back(std::move(ranged));
          }
        }
      }
    }

    if (!index_sets.empty()) {
      // Intersect candidate sets, starting from the smallest set for minimal memory/CPU overhead
      std::sort(index_sets.begin(), index_sets.end(), [](const auto& a, const auto& b) {
        return a.size() < b.size();
      });

      candidates = std::move(index_sets[0]);
      for (size_t si = 1; si < index_sets.size(); ++si) {
        if (candidates.empty()) break;
        std::unordered_set<ObjectID, TypedIdHash<9>> filter_set(index_sets[si].begin(), index_sets[si].end());
        std::vector<ObjectID> intersected;
        intersected.reserve(std::min(candidates.size(), index_sets[si].size()));
        for (const auto& id : candidates) {
          if (filter_set.count(id)) intersected.push_back(id);
        }
        candidates = std::move(intersected);
      }

      if (query.count_only && groups[0].size() == index_sets.size() && as_of_none && !query.has_near &&
          !query.has_related && !query.has_history) {
        res.count = candidates.size();
        res.ids = std::move(candidates);
        res.formatted = "COUNT(*) = " + std::to_string(res.count) + "\n";
        return res;
      }
    } else if (!ordered_walk_possible) {
      candidates = store.list();
    }
  } else if (!ordered_walk_possible) {
    candidates = store.list();
  }

  // 2. Resolve AS OF Checkpoint timestamp if requested. One that resolves to
  // nothing is an error: running the query unbounded would answer a different
  // question than the one asked.
  uint64_t as_of_ts = 0;
  if (query.has_as_of) {
    std::string tried;
    as_of_ts = resolve_checkpoint_timestamp(host, root_dir, query, &tried);
    if (as_of_ts == 0) {
      res.success = false;
      res.error = "AS OF names no time this machine can resolve" +
                  (tried.empty() ? std::string(" (a timestamp must be a positive number of nanoseconds)")
                                 : " — no readable, verified checkpoint at " + tried);
      return res;
    }
  }

  // 3. Filter Candidates.
  //
  // Nothing is copied here. This used to copy every matching object — all of
  // its metadata strings — and then sort and truncate that; a LIMIT 10 over a
  // 350k corpus copied 350k rows to keep ten. Now the pass keeps an id, a
  // timestamp, and (only when ordering needs one) a single sort key, and the
  // objects are materialised at the end for the rows that survived.
  const bool order_by_ts = query.order_by.empty() || query.order_by == "timestamp" ||
                           query.order_by == "timestamp_ns";
  struct Hit {
    ObjectID id;
    uint64_t ts = 0;
    std::string key;
    double num = 0;        // `key` parsed once, when it is a number
    bool numeric = false;
  };
  // ORDER BY a numeric key with a LIMIT: walk the ordering from the right end
  // and stop once enough rows have passed the filter, instead of materialising
  // everything and sorting it. This is the whole of the 283x gap against a
  // B-tree on `ORDER BY price DESC LIMIT 10`.
  if (!order_by_ts && query.limit > 0 && query.order_by.rfind("meta.", 0) == 0 &&
      !query.has_near && !query.has_related && !query.has_history) {
    const std::string ok = query.order_by.substr(5);
    std::vector<ObjectID> ordered;
    if (store.numeric_ordered(ok) && store.order_by_meta(ok, query.order_desc, 0, ordered)) {
      // Restrict to the candidate set when one was chosen, keeping index order.
      if (!candidates.empty() && candidates.size() < ordered.size()) {
        std::unordered_set<ObjectID, TypedIdHash<9>> keepset(candidates.begin(), candidates.end());
        std::vector<ObjectID> narrowed;
        narrowed.reserve(std::min(candidates.size(), ordered.size()));
        for (const auto& id : ordered) if (keepset.count(id)) narrowed.push_back(id);
        ordered.swap(narrowed);
      }
      std::vector<StoredObject> taken;
      const size_t want = query.limit;
      // Walk in page-sized runs so a filter that rejects most rows does not
      // turn into one read per row.
      const size_t kRun = std::max<size_t>(want * 8, 256);
      for (size_t at = 0; at < ordered.size() && taken.size() < want; at += kRun) {
        std::vector<ObjectID> slice(ordered.begin() + at,
                                    ordered.begin() + std::min(at + kRun, ordered.size()));
        store.visit(slice, [&](const StoredObject& o) {
          if (taken.size() >= want) return;
          if (as_of_ts > 0 && o.timestamp_ns > as_of_ts) return;
          for (const auto& group : groups) {
            bool all = true;
            for (const auto& p : group) {
              bool hit = evaluate_predicate(o, p);
              if (p.negated) hit = !hit;
              if (!hit) { all = false; break; }
            }
            if (all) { taken.push_back(o); return; }
          }
        });
      }
      res.count = taken.size();
      if (query.count_only) {
        res.formatted = "COUNT(*) = " + std::to_string(res.count) + "\n";
        return res;
      }
      res.objects = std::move(taken);
      aios::StrBuf oss;
      oss << "AQL Query Results (" << res.count << " object(s)):\n";
      size_t shown_o = 0;
      for (const auto& obj : res.objects) {
        if (shown_o++ == kFormatCap) { oss << "  ... and " << (res.count - kFormatCap) << " more\n"; break; }
        oss << "  [" << obj.id.short_str() << "] ts=" << obj.timestamp_ns << "\n";
      }
      res.formatted = oss.str();
      return res;
    }
  }

  std::vector<Hit> hits;
  store.visit(candidates, [&](const StoredObject& o) {
    // Time Travel filtering
    if (as_of_ts > 0 && o.timestamp_ns > as_of_ts) return;

    // Predicate evaluation: any group matching in full is a match.
    bool ok = false;
    for (const auto& group : groups) {
      bool all = true;
      for (const auto& p : group) {
        bool hit = evaluate_predicate(o, p);
        if (p.negated) hit = !hit;
        if (!hit) { all = false; break; }
      }
      if (all) { ok = true; break; }
    }
    if (!ok) return;

    Hit h;
    h.id = o.id;
    h.ts = o.timestamp_ns;
    if (!order_by_ts) {
      h.key = field_value(o, query.order_by);
      // Parsed here, once per row. The comparator used to call strtod on both
      // sides of every comparison, so ordering 20k rows meant ~560k parses of
      // the same 20k strings.
      h.numeric = numeric_of(h.key, &h.num);
    }
    hits.push_back(std::move(h));
  });

  // A COUNT is answered here, before anything is ordered or materialised. The
  // identities travel with it so a capability-scoped caller can narrow the
  // count without ever loading a row.
  if (query.count_only) {
    res.count = hits.size();
    res.ids.reserve(hits.size());
    for (const auto& h : hits) res.ids.push_back(h.id);
    res.formatted = "COUNT(*) = " + std::to_string(res.count) + "\n";
    return res;
  }

  // 4. Order By. Any field the filter understands can order the result, which
  // is the whole point of asking for the dearest or the oldest.
  //
  // NEAR without an ORDER BY keeps the similarity ranking: `hits` is already
  // in find_similar's order (visit serves ids in the order asked), and
  // re-sorting it by time made LIMIT keep the newest candidates, not the nearest.
  const bool keep_similarity_order = query.has_near && !query.order_explicit;
  if (!keep_similarity_order) std::sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) {
    if (order_by_ts) {
      return query.order_desc ? (a.ts > b.ts) : (a.ts < b.ts);
    }
    int cmp;
    if (a.numeric && b.numeric) {
      cmp = a.num < b.num ? -1 : (a.num > b.num ? 1 : 0);
    } else {
      cmp = a.key < b.key ? -1 : (a.key > b.key ? 1 : 0);
    }
    // A stable tiebreak, so equal keys do not reorder run to run.
    if (cmp == 0) return a.ts > b.ts;
    return query.order_desc ? (cmp > 0) : (cmp < 0);
  });

  // 5. Limit
  if (query.limit > 0 && hits.size() > query.limit) {
    hits.resize(query.limit);
  }

  // 6. Materialise only the rows that survived the limit.
  std::vector<ObjectID> keep;
  keep.reserve(hits.size());
  for (const auto& h : hits) keep.push_back(h.id);
  res.objects.reserve(keep.size());
  store.visit(keep, [&](const StoredObject& o) { res.objects.push_back(o); });
  res.count = res.objects.size();

  // Formatted Output. Capped: this used to render every matched object into a
  // string on every query, so a scan matching 20k rows built a multi-megabyte
  // listing that most callers (which read .objects or .count) never looked at.
  aios::StrBuf ss;
  ss << "AQL Query Results (" << res.count << " object(s)):\n";
  size_t shown = 0;
  for (const auto& obj : res.objects) {
    if (shown++ == kFormatCap) {
      ss << "  ... and " << (res.count - kFormatCap) << " more\n";
      break;
    }
    ss << "  [" << obj.id.short_str() << "] ts=" << obj.timestamp_ns;
    if (!obj.metadata.empty()) {
      ss << " meta={";
      for (size_t i = 0; i < obj.metadata.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << obj.metadata[i].first << "=" << obj.metadata[i].second;
      }
      ss << "}";
    }
    if (!obj.payload.empty()) {
      std::string text(obj.payload.begin(), obj.payload.end());
      if (text.size() > 40) text = text.substr(0, 37) + "...";
      ss << " payload=\"" << text << "\"";
    }
    ss << "\n";
  }
  res.formatted = ss.str();
  return res;
}

AqlQueryResult execute_aql(Surface& surface, const AqlQuery& query) {
  const ObjectStore& store = surface.kernel().store();
  hal::Host& host = surface.kernel().host();
  std::string root = surface.kernel().config().root;

  // A write goes through the Surface like every other write: it needs
  // storage write authority and the object records who wrote it. Handing it to
  // the raw store let read-only access write, with no provenance.
  if (query.verb == AqlVerb::Put) {
    AqlQueryResult r;
    auto id = surface.store(query.put_object);
    if (!id.ok()) { r.success = false; r.error = id.error().str(); return r; }
    auto got = store.get(*id);
    if (got.ok()) r.objects.push_back(*got);
    r.count = r.objects.size();
    r.formatted = "Inserted object " + id->str() + "\n";
    return r;
  }

  // A COUNT still has to be scoped before it is counted — the answer is how
  // many objects THIS entity may read, not how many exist. But a capability
  // check takes an id, so the count is scoped without loading a single row.
  if (query.count_only) {
    AqlQueryResult raw = execute_aql(store, query, &host, root);
    if (!raw.success) return raw;
    AqlQueryResult counted;
    counted.success = true;
    for (const auto& id : raw.ids) {
      if (surface.may(ResourceRef{ResourceKind::StorageObject, id}, R_Read).ok()) counted.count++;
    }
    counted.formatted = "COUNT(*) = " + std::to_string(counted.count) + "\n";
    return counted;
  }

  AqlQueryResult raw = execute_aql(store, query, &host, root);
  if (!raw.success) return raw;

  // Apply Capability Scoping: Only return objects the calling entity surface can read!
  AqlQueryResult scoped;
  scoped.success = true;
  scoped.objects.reserve(raw.objects.size());
  for (auto& obj : raw.objects) {
    auto check = surface.may(ResourceRef{ResourceKind::StorageObject, obj.id}, R_Read);
    // Moved, not copied: `raw` is this function's own result and is finished
    // with. Copying here meant every scoped query paid for its rows twice.
    if (check.ok()) scoped.objects.push_back(std::move(obj));
  }
  scoped.count = scoped.objects.size();

  aios::StrBuf ss;
  ss << "AQL Capability-Scoped Results (" << scoped.count << " object(s)):\n";
  size_t shown = 0;
  for (const auto& obj : scoped.objects) {
    if (shown++ == kFormatCap) {
      ss << "  ... and " << (scoped.count - kFormatCap) << " more\n";
      break;
    }
    ss << "  [" << obj.id.short_str() << "] ts=" << obj.timestamp_ns;
    if (!obj.metadata.empty()) {
      ss << " meta={";
      for (size_t i = 0; i < obj.metadata.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << obj.metadata[i].first << "=" << obj.metadata[i].second;
      }
      ss << "}";
    }
    ss << "\n";
  }
  scoped.formatted = ss.str();
  return scoped;
}

AqlQueryResult execute_aql(Surface& surface, const std::string& query_str) {
  auto q = parse_aql(query_str);
  if (!q.ok()) {
    AqlQueryResult r; r.success = false; r.error = q.error().str(); return r;
  }
  return execute_aql(surface, *q);
}

AqlQueryResult execute_aql(const ObjectStore& store, const std::string& query_str, hal::Host* host, const std::string& root_dir) {
  auto q = parse_aql(query_str);
  if (!q.ok()) {
    AqlQueryResult r; r.success = false; r.error = q.error().str(); return r;
  }
  return execute_aql(store, *q, host, root_dir);
}

}  // namespace aios::storage
