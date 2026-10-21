#include "aios/hal/host.hpp"
#include "aios/intelligence/qwen_mind.hpp"
#include "aios/userland/llm.hpp"
#include "aios/channel/http_client.hpp"
#include "aios/core/strbuf.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/kernel/surface.hpp"
#include "aios/compute/backends.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace aios::intelligence {

std::string network_mind_credential(hal::Host& host, const std::string& kernel_root) {
  auto trim = [](std::string v) {
    size_t b = v.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = v.find_last_not_of(" \t\r\n");
    return v.substr(b, e - b + 1);
  };
  if (!kernel_root.empty()) {
    auto f = host.read_file(kernel_root + "/mind.key");
    if (f.ok() && f->size() > 0) {
      return trim(std::string((const char*)f->data(), f->size()));
    }
  }
  if (std::string e = hal::process_host().env("AIOS_MIND_KEY"); !e.empty()) return trim(e);
  return std::string();
}

QwenClient::QwenClient(std::string url, std::string model)
    : url_(std::move(url)), model_(std::move(model)) {
  if (url_.empty()) {
    const std::string env_s = hal::process_host().env("AIOS_MIND_URL");
    const char* env = env_s.empty() ? nullptr : env_s.c_str();
    if (env && *env) url_ = env;
  }
}

void QwenClient::set_endpoint(const std::string& url, const std::string& model) {
  url_ = url;
  model_ = model;
}

Result<std::string> QwenClient::chat(
    const std::vector<std::pair<std::string, std::string>>& messages,
    hal::Host* host, const std::string& kernel_root, int timeout_ms) const {
  if (url_.empty()) {
    return Result<std::string>(Status::Unsupported,
        "no model endpoint configured: set AIOS_MIND_URL (e.g. http://<host>:8000/v1/chat/completions)");
  }
  if (url_ == "local" || url_ == "cuda" || url_ == "in-process") {
    // The in-tree model (llm.hpp), on this machine's own lanes: no server
    // between the mind and the weights.
    hal::Host& h = host ? *host : hal::process_host();
    auto m = userland::resident_llm(h, kernel_root);
    if (!m.ok()) return Result<std::string>(m.error());
    std::vector<userland::ChatMessage> turns;
    for (const auto& [role, content] : messages) turns.push_back({role, content});
    userland::SampleParams sp = (*m)->defaults();
    sp.max_tokens = max_tokens_ > 0 ? (uint32_t)max_tokens_ : 0;
    const uint64_t deadline = timeout_ms > 0 ? h.monotonic_ns() + (uint64_t)timeout_ms * 1000000ull : 0;
    auto g = (*m)->generate(h, (*m)->chat_prompt(turns), sp,
                            [&](const std::string&) { return !deadline || h.monotonic_ns() < deadline; });
    if (!g.ok()) return Result<std::string>(g.error());
    return g->text;
  }

  auto ep = channel::parse_endpoint(url_);
  if (!ep.ok()) return Result<std::string>(ep.error());

  channel::RpcValue req_obj = channel::RpcValue::object();
  req_obj.set("model", channel::RpcValue::text(model_));

  channel::RpcValue msg_list = channel::RpcValue::list();
  for (const auto& [role, content] : messages) {
    channel::RpcValue m = channel::RpcValue::object();
    m.set("role", channel::RpcValue::text(role));
    m.set("content", channel::RpcValue::text(content));
    msg_list.push(std::move(m));
  }
  req_obj.set("messages", std::move(msg_list));
  // Anthropic-compatible and several others refuse a request that does not
  // bound the reply; OpenAI itself does not mind it being present.
  if (max_tokens_ > 0) req_obj.set("max_tokens", channel::RpcValue::number((uint64_t)max_tokens_));

  std::string body_data = req_obj.render();

  channel::HttpRequest req;
  req.method = "POST";
  req.endpoint = *ep;
  req.timeout_ms = timeout_ms;
  req.host = host;
  req.kernel_root = kernel_root;
  req.headers.emplace_back("Content-Type", "application/json");
  req.headers.emplace_back("Accept", "application/json");
  // The whole reason this client could not reach a real model API.
  if (!key_.empty()) req.headers.emplace_back("Authorization", "Bearer " + key_);
  req.body.assign(body_data.begin(), body_data.end());

  auto resp = channel::http_send(req);
  if (!resp.ok()) return Result<std::string>(resp.error());
  if (resp->status != 200) {
    // WHAT it said, not just that it said no. A bare "returned status 401"
    // cannot be told apart from a wrong URL, a missing credential, a model
    // name the endpoint does not have, or a refused region — and every one of
    // those is a different fix.
    std::string why = resp->text();
    if (why.size() > 400) why = why.substr(0, 400) + "…";
    for (char& ch : why) if (ch == '\n' || ch == '\r') ch = ' ';
    return Result<std::string>(Status::IoError,
        "the model endpoint returned status " + std::to_string(resp->status) +
        (key_.empty() ? " (no credential was sent; put one in <root>/mind.key)" : "") +
        (why.empty() ? "" : ": " + why));
  }

  auto parsed = channel::rpc_parse(ByteSpan(resp->body.data(), resp->body.size()));
  if (!parsed.ok()) return Result<std::string>(parsed.error());

  const auto& choices = (*parsed)["choices"];
  if (choices.size() > 0) {
    const auto& c0 = choices[0];
    if (c0.has("message") && c0["message"].has("content")) {
      return c0["message"]["content"].as_text();
    }
    if (c0.has("text")) {
      return c0["text"].as_text();
    }
  }

  return Result<std::string>(Status::Corrupt, "invalid response payload");
}

Result<std::string> QwenClient::complete(const std::string& input_text,
                                         hal::Host* host, const std::string& kernel_root,
                                         int timeout_ms) const {
  std::vector<std::pair<std::string, std::string>> msgs = {{"user", input_text}};
  return chat(msgs, host, kernel_root, timeout_ms);
}

Result<std::string> QwenClient::translate_command(const std::string& input_text,
                                                  hal::Host* host, const std::string& kernel_root,
                                                  int timeout_ms) const {
  std::string query_seq = "Translate this natural language string into a valid system command: " + input_text;
  return complete(query_seq, host, kernel_root, timeout_ms);
}

namespace {
// The model gets ONE fixed directive syntax it may use to ask the machine to
// actually compute something, rather than just talk about it: a first line
// of exactly "CALC: add <a> <b>" or "CALC: mul <a> <b>". Anything else is
// a plain reply -- no directive, no graph, nothing executes. Parsing the
// model's own reply (not the caller's input) means the model decides
// whether the request needs real compute; the directive line is stripped
// from what act() keeps as the visible answer.
bool parse_calc_line(const std::string& text, std::string& op, double& a, double& b, std::string& rest) {
  size_t nl = text.find('\n');
  std::string first = text.substr(0, nl == std::string::npos ? text.size() : nl);
  size_t s = first.find_first_not_of(" \t");
  if (s == std::string::npos) return false;
  first = first.substr(s);
  static const std::string kPrefix = "CALC:";
  if (first.size() < kPrefix.size()) return false;
  std::string head = first.substr(0, kPrefix.size());
  std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return std::toupper(c); });
  if (head != kPrefix) return false;
  {
    std::vector<std::string> w;
    for (const auto& piece : aios::split_lines(first.substr(kPrefix.size()), ' ')) if (!piece.empty()) w.push_back(piece);
    if (w.size() < 3) return false;
    op = w[0];
    const char* e1 = nullptr; const char* e2 = nullptr;
    a = aios::to_f64(w[1].c_str(), &e1);
    b = aios::to_f64(w[2].c_str(), &e2);
    if (e1 == w[1].c_str() || e2 == w[2].c_str()) return false;
  }
  std::transform(op.begin(), op.end(), op.begin(), [](unsigned char c) { return std::tolower(c); });
  if (op != "add" && op != "mul") return false;
  rest = (nl == std::string::npos) ? std::string() : text.substr(nl + 1);
  return true;
}
}  // namespace

QwenIntelligence::QwenIntelligence(std::string url, std::string model)
    : client_(std::move(url), std::move(model)) {}

std::string QwenIntelligence::name() const {
  // Says what answers: a model over the network ("net:"), or the in-tree one
  // in this process ("local:"), and which model that is.
  const std::string& u = client_.url();
  const bool local = u == "local" || u == "cuda" || u == "in-process";
  return (local ? "local:" : "net:") + client_.model();
}

Result<StateID> QwenIntelligence::perceive(Surface& s, const StateObject& context) {
  last_input_ = std::string(context.payload.begin(), context.payload.end());
  if (last_input_.empty()) {
    last_input_ = "Hello from AIOS";
  }
  return s.create_state(StateKind::Working, ByteSpan(reinterpret_cast<const uint8_t*>(last_input_.data()), last_input_.size()));
}

Result<GraphID> QwenIntelligence::think(Surface& s, const StateObject& perceived) {
  has_calc_ = false;
  std::vector<std::pair<std::string, std::string>> msgs = {
    {"system", "If the user's message asks you to add or multiply two numbers, reply with a "
               "first line that is EXACTLY 'CALC: add <a> <b>' or 'CALC: mul <a> <b>' using the "
               "actual numbers, then explain on the following lines. For anything else, just "
               "answer normally with no CALC line."},
    {"user", last_input_},
  };
  auto res = client_.chat(msgs, &s.kernel().host());
  if (!res.ok()) {
    // Named for what it is. "Qwen intelligence error" appeared in front of a
    // failure from an OpenAI endpoint, which sends whoever reads it looking at
    // the wrong thing entirely.
    last_response_ = "the network mind could not reach " + client_.url() + ": " + res.error().message;
    return GraphID();
  }
  last_response_ = *res;

  std::string op, rest;
  double a = 0, b = 0;
  if (!parse_calc_line(last_response_, op, a, b, rest)) {
    return GraphID();  // a plain reply: nothing to compute, nothing executes
  }

  // The model asked for real compute. From here every step is a normal,
  // capability-checked Surface call -- if this entity's own authority does
  // not cover it, the call fails right here (or execute() fails inside
  // Kernel::cycle, before act() ever runs) and the whole cycle errors out
  // cleanly. Nothing about this path grants anything; it only spends
  // authority the entity already holds.
  auto ta = s.tensor(DType::F64, {1});
  if (!ta.ok()) return ta.error();
  auto tb = s.tensor(DType::F64, {1});
  if (!tb.ok()) { (void)s.release(ta->memory); return tb.error(); }
  auto tout = s.tensor(DType::F64, {1});
  if (!tout.ok()) { (void)s.release(ta->memory); (void)s.release(tb->memory); return tout.error(); }

  auto g = s.create_graph();
  if (!g.ok()) return g.error();

  Operation fill_a; fill_a.code = OpCode::Fill; fill_a.outputs = {*ta}; fill_a.scalar = a;
  Operation fill_b; fill_b.code = OpCode::Fill; fill_b.outputs = {*tb}; fill_b.scalar = b;
  Operation calc;   calc.code = (op == "mul") ? OpCode::Mul : OpCode::Add;
  calc.inputs = {*ta, *tb}; calc.outputs = {*tout};

  auto op_a = s.add_operation(*g, fill_a);
  if (!op_a.ok()) return op_a.error();
  auto op_b = s.add_operation(*g, fill_b);
  if (!op_b.ok()) return op_b.error();
  auto op_c = s.add_operation(*g, calc);
  if (!op_c.ok()) return op_c.error();

  calc_out_ = *tout;
  calc_a_ = a;
  calc_b_ = b;
  calc_expr_ = (op == "mul") ? " * " : " + ";
  has_calc_ = true;
  last_response_ = rest;  // the model's explanation, directive line stripped
  return *g;
}

Result<StateID> QwenIntelligence::act(Surface& s, const StateObject& perceived,
                                       const ComputeGraph* graph,
                                       const ExecutionReport* report) {
  if (has_calc_ && report && report->complete) {
    auto bytes = s.read(calc_out_.memory, calc_out_.offset, calc_out_.bytes());
    if (bytes.ok() && bytes->size() >= sizeof(double)) {
      double result = 0;
      std::memcpy(&result, bytes->data(), sizeof(double));
      aios::StrBuf oss;
      oss << last_response_ << "\n[AIOS executed a real compute graph: "
          << calc_a_ << calc_expr_ << calc_b_ << " = " << result << "]";
      last_response_ = oss.str();
    }
    has_calc_ = false;
  }
  return s.create_state(StateKind::Working, ByteSpan(reinterpret_cast<const uint8_t*>(last_response_.data()), last_response_.size()));
}

std::unique_ptr<Intelligence> make_qwen_intelligence(const std::string& url,
                                                      const std::string& model) {
  return std::make_unique<QwenIntelligence>(url, model);
}

void set_qwen_credential(Intelligence& mind, const std::string& bearer) {
  // Only a network mind has one; anything else is left alone rather than
  // refused, so a caller can offer a credential without knowing the kind.
  if (auto* q = dynamic_cast<QwenIntelligence*>(&mind)) q->client().set_credential(bearer);
}

}  // namespace aios::intelligence
