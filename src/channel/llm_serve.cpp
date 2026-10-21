#include "aios/channel/llm_serve.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/hal/net.hpp"
#include "aios/hal/sync.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <memory>

namespace aios::channel {
namespace {

struct Request {
  std::string method, path, body;
  std::string authorization;
};

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool constant_time_eq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff |= (uint8_t)a[i] ^ (uint8_t)b[i];
  return diff == 0;
}

// Reads one HTTP/1.1 request: the head up to the blank line, then
// Content-Length bytes. Anything it cannot read whole is refused, not guessed at.
Result<Request> read_request(hal::Connection& c, const LlmServeOptions& o) {
  std::string buf;
  size_t head_end = std::string::npos;
  char tmp[4096];
  while (head_end == std::string::npos) {
    if (buf.size() > 64 * 1024) return Result<Request>(Status::Invalid, "request head is too large");
    long n = c.recv_some(MutableByteSpan(reinterpret_cast<uint8_t*>(tmp), sizeof tmp), o.io_timeout_ms);
    if (n <= 0) return Result<Request>(Status::Invalid, "connection closed before a full request head arrived");
    buf.append(tmp, (size_t)n);
    head_end = buf.find("\r\n\r\n");
  }
  Request r;
  const std::string head = buf.substr(0, head_end);
  size_t line_end = head.find("\r\n");
  const std::string first = head.substr(0, line_end);
  const size_t s1 = first.find(' '), s2 = first.find(' ', s1 + 1);
  if (s1 == std::string::npos || s2 == std::string::npos) return Result<Request>(Status::Invalid, "malformed request line");
  r.method = first.substr(0, s1);
  r.path = first.substr(s1 + 1, s2 - s1 - 1);
  if (const size_t q = r.path.find('?'); q != std::string::npos) r.path.resize(q);
  size_t content_length = 0;
  bool chunked = false;
  for (size_t p = line_end; p != std::string::npos && p < head.size();) {
    const size_t e = head.find("\r\n", p + 2);
    const std::string line = head.substr(p + 2, (e == std::string::npos ? head.size() : e) - p - 2);
    p = e;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string k = lower(line.substr(0, colon));
    std::string v = line.substr(colon + 1);
    v.erase(0, v.find_first_not_of(" \t"));
    if (k == "content-length") {
      size_t n = 0;
      bool digits = !v.empty();
      for (char ch : v) {
        if (ch == ' ' || ch == '\t') break;
        if (ch < '0' || ch > '9' || n > (SIZE_MAX - 9) / 10) { digits = false; break; }
        n = n * 10 + (size_t)(ch - '0');
      }
      if (!digits) return Result<Request>(Status::Invalid, "bad Content-Length");
      content_length = n;
    }
    else if (k == "transfer-encoding" && lower(v).find("chunked") != std::string::npos) chunked = true;
    else if (k == "authorization") r.authorization = v;
  }
  if (chunked) return Result<Request>(Status::Unsupported, "chunked request bodies are not supported; send Content-Length");
  if (content_length > o.max_body) return Result<Request>(Status::Invalid, "request body is too large");
  r.body = buf.substr(head_end + 4);
  while (r.body.size() < content_length) {
    long n = c.recv_some(MutableByteSpan(reinterpret_cast<uint8_t*>(tmp), std::min(sizeof tmp, content_length - r.body.size())), o.io_timeout_ms);
    if (n <= 0) return Result<Request>(Status::Invalid, "connection closed before the body arrived");
    r.body.append(tmp, (size_t)n);
  }
  r.body.resize(content_length);
  return r;
}

const char* reason(int code) {
  switch (code) {
    case 200: return "OK"; case 400: return "Bad Request"; case 401: return "Unauthorized";
    case 404: return "Not Found"; case 405: return "Method Not Allowed"; case 413: return "Payload Too Large";
    case 500: return "Internal Server Error"; default: return "Error";
  }
}

void respond(hal::Connection& c, int code, const std::string& body, const char* type = "application/json") {
  std::string h = "HTTP/1.1 " + std::to_string(code) + " " + reason(code) + "\r\nContent-Type: " + type +
                  "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n";
  if (code == 401) h += "WWW-Authenticate: Bearer\r\n";
  h += "\r\n" + body;
  c.send_all(span_of(h));
}

void respond_error(hal::Connection& c, int code, const std::string& msg, const char* type = "invalid_request_error") {
  RpcValue e = RpcValue::object();
  e.set("message", RpcValue::text(msg));
  e.set("type", RpcValue::text(type));
  RpcValue o = RpcValue::object();
  o.set("error", std::move(e));
  respond(c, code, o.render());
}

std::string message_text(const RpcValue& content) {
  if (content.kind() == RpcValue::Kind::Text) return content.as_text();
  std::string s;   // a list of parts: the text ones, in order
  for (const auto& part : content.items())
    if (part["type"].as_text() == "text") s += part["text"].as_text();
  return s;
}

std::string completion_id(hal::Host& host) {
  uint8_t b[6];
  host.random(MutableByteSpan(b, 6));
  static const char* d = "0123456789abcdef";
  std::string s = "chatcmpl-";
  for (uint8_t x : b) { s.push_back(d[x >> 4]); s.push_back(d[x & 15]); }
  return s;
}

RpcValue usage_of(const userland::GenerateResult& g) {
  RpcValue u = RpcValue::object();
  u.set("prompt_tokens", RpcValue::number((uint64_t)g.prompt_tokens));
  u.set("completion_tokens", RpcValue::number((uint64_t)g.completion_tokens));
  u.set("total_tokens", RpcValue::number((uint64_t)g.prompt_tokens + g.completion_tokens));
  return u;
}

RpcValue chunk_of(const std::string& id, uint64_t created, const std::string& model, const RpcValue& delta, const char* finish) {
  RpcValue choice = RpcValue::object();
  choice.set("index", RpcValue::number((uint64_t)0));
  choice.set("delta", delta);
  choice.set("finish_reason", finish ? RpcValue::text(finish) : RpcValue());
  RpcValue choices = RpcValue::list();
  choices.push(std::move(choice));
  RpcValue o = RpcValue::object();
  o.set("id", RpcValue::text(id));
  o.set("object", RpcValue::text("chat.completion.chunk"));
  o.set("created", RpcValue::number(created));
  o.set("model", RpcValue::text(model));
  o.set("choices", std::move(choices));
  return o;
}

void handle_chat(userland::LlmModel& model, hal::Host& host, const LlmServeOptions& o, hal::Mutex& gen_lock,
                 hal::Connection& c, const Request& rq, const std::function<void(const std::string&)>& log) {
  auto parsed = rpc_parse(rq.body);
  if (!parsed.ok() || parsed->kind() != RpcValue::Kind::Object) return respond_error(c, 400, "the body is not a JSON object");
  const RpcValue& req = *parsed;
  std::vector<userland::ChatMessage> msgs;
  for (const auto& m : req["messages"].items()) {
    const std::string role = m["role"].as_text();
    if (role != "system" && role != "user" && role != "assistant")
      return respond_error(c, 400, "message role \"" + role + "\" is not one of system, user, assistant");
    msgs.push_back({role, message_text(m["content"])});
  }
  if (msgs.empty()) return respond_error(c, 400, "\"messages\" must hold at least one message");

  // What the request leaves out comes from the checkpoint's generation_config.json.
  userland::SampleParams sp = model.defaults();
  auto given = [&](const char* k) { return req.has(k) && !req[k].is_null(); };
  if (given("temperature")) sp.temperature = (float)req["temperature"].as_double(sp.temperature);
  if (given("top_p")) sp.top_p = (float)req["top_p"].as_double(sp.top_p);
  if (given("top_k")) sp.top_k = (uint32_t)req["top_k"].as_u64(sp.top_k);
  if (given("repetition_penalty")) sp.repetition_penalty = (float)req["repetition_penalty"].as_double(sp.repetition_penalty);
  if (given("seed")) sp.seed = req["seed"].as_u64();
  const char* limit_key = req.has("max_completion_tokens") ? "max_completion_tokens" : "max_tokens";
  sp.max_tokens = req.has(limit_key) && !req[limit_key].is_null() ? (uint32_t)req[limit_key].as_u64(256) : 256;
  if (req.has("stop")) {
    if (req["stop"].kind() == RpcValue::Kind::Text) sp.stop.push_back(req["stop"].as_text());
    else for (const auto& s : req["stop"].items()) sp.stop.push_back(s.as_text());
  }
  const bool stream = req.has("stream") && req["stream"].truthy();
  const std::string id = completion_id(host);
  const uint64_t created = host.wall_ns() / 1000000000ull;
  const std::string prompt = model.chat_prompt(msgs);

  hal::LockGuard guard(gen_lock);
  if (!stream) {
    // Nothing is sent until the reply is whole, so the only sign of a caller
    // that gave up is its end of the socket closing: look for that as the
    // text grows, and stop rather than hold the model for nobody.
    uint32_t pieces = 0;
    auto g = model.generate(host, prompt, sp, [&](const std::string&) {
      if (++pieces % 8) return true;
      uint8_t b;
      return !(c.recv_some(MutableByteSpan(&b, 1), 0) <= 0 && !c.alive());
    });
    if (g.ok() && g->cancelled) {
      log("chat abandoned by the caller after " + std::to_string(g->completion_tokens) + " tokens");
      return;
    }
    if (!g.ok()) return respond_error(c, g.error().status == Status::Invalid ? 400 : 500, g.error().message);
    RpcValue msg = RpcValue::object();
    msg.set("role", RpcValue::text("assistant"));
    msg.set("content", RpcValue::text(g->text));
    RpcValue choice = RpcValue::object();
    choice.set("index", RpcValue::number((uint64_t)0));
    choice.set("message", std::move(msg));
    choice.set("finish_reason", RpcValue::text(g->hit_limit ? "length" : "stop"));
    RpcValue choices = RpcValue::list();
    choices.push(std::move(choice));
    RpcValue out = RpcValue::object();
    out.set("id", RpcValue::text(id));
    out.set("object", RpcValue::text("chat.completion"));
    out.set("created", RpcValue::number(created));
    out.set("model", RpcValue::text(o.model_name));
    out.set("choices", std::move(choices));
    out.set("usage", usage_of(*g));
    respond(c, 200, out.render());
    log("chat " + std::to_string(g->prompt_tokens) + "+" + std::to_string(g->completion_tokens) + " tokens, prefill " +
        std::to_string((int)g->prefill_ms) + " ms, decode " + std::to_string((int)g->decode_ms) + " ms");
    return;
  }

  const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
  if (!c.send_all(span_of(head))) return;
  auto send_event = [&](const RpcValue& v) { const std::string e = "data: " + v.render() + "\n\n"; return c.send_all(span_of(e)); };
  RpcValue role = RpcValue::object();
  role.set("role", RpcValue::text("assistant"));
  if (!send_event(chunk_of(id, created, o.model_name, role, nullptr))) return;
  auto g = model.generate(host, prompt, sp, [&](const std::string& piece) {
    RpcValue delta = RpcValue::object();
    delta.set("content", RpcValue::text(piece));
    return send_event(chunk_of(id, created, o.model_name, delta, nullptr));   // a dropped client stops generation
  });
  if (!g.ok()) {
    RpcValue e = RpcValue::object();
    e.set("error", RpcValue::text(g.error().message));
    send_event(e);
  } else {
    send_event(chunk_of(id, created, o.model_name, RpcValue::object(), g->hit_limit ? "length" : "stop"));
    log("chat(stream) " + std::to_string(g->prompt_tokens) + "+" + std::to_string(g->completion_tokens) + " tokens");
  }
  const std::string done = "data: [DONE]\n\n";
  c.send_all(span_of(done));
}

}  // namespace

Result<void> llm_serve(userland::LlmModel& model, hal::Host& host, const LlmServeOptions& o,
                       std::function<void(const std::string&)> log, std::atomic<bool>* stop) {
  if (!o.loopback_only && o.api_key.empty())
    return Result<void>(Status::Denied, "refusing to listen on every interface with no API key");
  auto listener = hal::net_listen(o.port, o.loopback_only);
  if (!listener.ok()) return listener.error();
  log(std::string("llm endpoint listening on ") + (o.loopback_only ? "127.0.0.1" : "0.0.0.0") + ":" +
      std::to_string((*listener)->port()) + " model=" + o.model_name + (o.api_key.empty() ? " (no key)" : " (bearer key required)"));

  struct Handler {
    std::atomic<bool> finished{false};
    hal::Thread thread;
  };
  std::deque<std::unique_ptr<Handler>> handlers;
  hal::Mutex gen_lock;

  while (!(stop && stop->load())) {
    for (auto it = handlers.begin(); it != handlers.end();) {
      if ((*it)->finished.load()) it = handlers.erase(it); else ++it;
    }
    auto conn = (*listener)->accept(200);
    if (!conn.ok()) { if (conn.error().status == Status::Busy) continue; log("accept: " + conn.error().str()); hal::sleep_ms(50); continue; }
    std::shared_ptr<hal::Connection> c(std::move(*conn));
    auto h = std::make_unique<Handler>();
    Handler* hp = h.get();
    hp->thread = hal::Thread([&model, &host, &o, &gen_lock, &log, c, hp]() {
      auto rq = read_request(*c, o);
      if (!rq.ok()) {
        respond_error(*c, 400, rq.error().message);
      } else if (!o.api_key.empty() && !constant_time_eq(rq->authorization, "Bearer " + o.api_key)) {
        respond_error(*c, 401, "a valid bearer key is required", "authentication_error");
      } else if (rq->path == "/health") {
        respond(*c, 200, "{\"status\":\"ok\"}");
      } else if (rq->path == "/v1/models") {
        if (rq->method != "GET") { respond_error(*c, 405, "use GET"); }
        else {
          RpcValue m = RpcValue::object();
          m.set("id", RpcValue::text(o.model_name));
          m.set("object", RpcValue::text("model"));
          m.set("created", RpcValue::number(host.wall_ns() / 1000000000ull));
          m.set("owned_by", RpcValue::text("aios"));
          RpcValue data = RpcValue::list();
          data.push(std::move(m));
          RpcValue out = RpcValue::object();
          out.set("object", RpcValue::text("list"));
          out.set("data", std::move(data));
          respond(*c, 200, out.render());
        }
      } else if (rq->path == "/v1/chat/completions") {
        if (rq->method != "POST") respond_error(*c, 405, "use POST");
        else handle_chat(model, host, o, gen_lock, *c, *rq, log);
      } else {
        respond_error(*c, 404, "no such path: " + rq->path);
      }
      c->close();
      hp->finished.store(true);
    });
    handlers.push_back(std::move(h));
  }
  return Ok();
}

}  // namespace aios::channel
