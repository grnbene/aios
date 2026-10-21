// model_controller.cpp — see model_controller.h. Every model listed here was
// named by an endpoint that answered an HTTP request during this process.
#include "model_controller.h"
#include "aios/hal/sync.hpp"
#include "aios/channel/http_client.hpp"

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <mutex>

namespace aios::cli {
namespace {

// Minimal field extraction over the JSON these two APIs return. Neither
// nests the fields we read inside anything but the per-model object, so a
// scan per object is exact for them; this is not a JSON parser.
std::string field_str(const std::string& obj, const char* key) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = obj.find(k);
  if (p == std::string::npos) return {};
  p += k.size();
  while (p < obj.size() && (obj[p] == ' ')) ++p;
  if (p >= obj.size() || obj[p] != '"') return {};
  size_t e = obj.find('"', p + 1);
  if (e == std::string::npos) return {};
  return obj.substr(p + 1, e - p - 1);
}
uint64_t field_u64(const std::string& obj, const char* key) {
  std::string k = std::string("\"") + key + "\":";
  size_t p = obj.find(k);
  if (p == std::string::npos) return 0;
  p += k.size();
  while (p < obj.size() && obj[p] == ' ') ++p;
  return std::strtoull(obj.c_str() + p, nullptr, 10);
}
// Splits a JSON array of objects into the text of each top-level object.
std::vector<std::string> objects_of(const std::string& body, const char* array_key) {
  std::vector<std::string> out;
  std::string k = std::string("\"") + array_key + "\":";
  size_t p = body.find(k);
  if (p == std::string::npos) return out;
  p = body.find('[', p);
  if (p == std::string::npos) return out;
  int depth = 0; size_t start = 0; bool in_str = false;
  for (size_t i = p + 1; i < body.size(); ++i) {
    char c = body[i];
    if (in_str) { if (c == '\\') ++i; else if (c == '"') in_str = false; continue; }
    if (c == '"') { in_str = true; continue; }
    if (c == '{') { if (depth++ == 0) start = i; }
    else if (c == '}') { if (--depth == 0) out.push_back(body.substr(start, i - start + 1)); }
    else if (c == ']' && depth == 0) break;
  }
  return out;
}

std::string fmt_bytes(uint64_t b) {
  const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = (double)b; int i = 0;
  while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
  char buf[32]; std::snprintf(buf, sizeof buf, i ? "%.1f %s" : "%.0f %s", v, u[i]);
  return buf;
}

std::string label_of(const std::string& base) {
  auto ep = channel::parse_endpoint(base);
  if (!ep.ok()) return base;
  if (ep->host == "127.0.0.1" || ep->host == "localhost") return "this machine";
  return ep->host;
}

}  // namespace

std::vector<ModelEndpoint> model_endpoints() {
  std::vector<ModelEndpoint> out;
  if (const char* e = std::getenv("AIOS_MODEL_ENDPOINTS"); e && *e) {
    std::stringstream ss(e);
    std::string item;
    while (std::getline(ss, item, ',')) {
      size_t eq = item.find('=');
      if (eq == std::string::npos) continue;
      ModelEndpoint ep{item.substr(0, eq), item.substr(eq + 1), ""};
      ep.label = label_of(ep.base);
      out.push_back(ep);
    }
    if (!out.empty()) return out;
  }
  // No other machine is built in: this machine's ollama, plus the endpoint
  // AIOS_MIND_URL names, if any. Every other host comes from
  // AIOS_MODEL_ENDPOINTS (kind=base,...), above.
  out.push_back({"ollama", "http://127.0.0.1:11434", "this machine"});
  if (const char* u = std::getenv("AIOS_MIND_URL"); u && *u) {
    std::string base = u;
    size_t v1 = base.find("/v1/");
    if (v1 != std::string::npos) base = base.substr(0, v1);
    ModelEndpoint ep{"openai", base, ""};
    ep.label = label_of(ep.base);
    out.push_back(ep);
  }
  return out;
}

size_t ModelController::discover(int timeout_ms) {
  models_.clear();
  endpoints_.clear();
  size_t answered = 0;
  for (const auto& ep : model_endpoints()) {
    EndpointStatus st;
    st.endpoint = ep;
    const std::string list_url = ep.base + (ep.kind == "ollama" ? "/api/tags" : "/v1/models");
    auto r = channel::http_get(list_url, timeout_ms);
    if (!r.ok()) { st.detail = r.error().message; endpoints_.push_back(st); continue; }
    if (r->status != 200) { st.detail = "HTTP " + std::to_string(r->status); endpoints_.push_back(st); continue; }
    st.reachable = true;
    st.detail = "HTTP 200";
    ++answered;
    const std::string body = r->text();
    const std::string chat = ep.base + "/v1/chat/completions";
    if (ep.kind == "ollama") {
      for (const auto& obj : objects_of(body, "models")) {
        ModelInfo m;
        m.name = field_str(obj, "name");
        if (m.name.empty()) continue;
        m.provider = "ollama";
        m.endpoint = chat;
        m.host = ep.label;
        m.parameters = field_str(obj, "parameter_size");
        m.bytes = field_u64(obj, "size");
        models_.push_back(m);
        ++st.models;
      }
      // What ollama holds in memory right now, which is the honest meaning of "loaded".
      auto ps = channel::http_get(ep.base + "/api/ps", timeout_ms);
      if (ps.ok() && ps->status == 200) {
        for (const auto& obj : objects_of(ps->text(), "models")) {
          std::string n = field_str(obj, "name");
          for (auto& m : models_) if (m.endpoint == chat && m.name == n) m.loaded = true;
        }
      }
    } else {
      for (const auto& obj : objects_of(body, "data")) {
        ModelInfo m;
        m.name = field_str(obj, "id");
        if (m.name.empty()) continue;
        bool dup = false;
        for (auto& x : models_) if (x.endpoint == chat && x.name == m.name) dup = true;
        if (dup) continue;
        m.provider = "openai";
        m.endpoint = chat;
        m.host = ep.label;
        m.loaded = true;   // a vLLM/transformers server lists only what it has resident
        models_.push_back(m);
        ++st.models;
      }
    }
    endpoints_.push_back(st);
  }
  return answered;
}

std::string ModelController::endpoint_for(const std::string& name, std::string* where) const {
  for (const auto& m : models_) {
    if (m.name == name || (m.name.rfind(name + ":", 0) == 0)) {
      if (where) *where = m.host;
      return m.endpoint;
    }
  }
  return {};
}

std::string ModelController::render() const {
  std::ostringstream o;
  o << "models this machine can reach right now:\n";
  if (models_.empty()) o << "  (none — no endpoint answered with a model)\n";
  for (const auto& m : models_) {
    o << "  " << m.name;
    if (!m.parameters.empty()) o << "  " << m.parameters;
    if (m.bytes) o << "  " << fmt_bytes(m.bytes);
    o << "  via " << m.provider << " on " << m.host << (m.loaded ? "  [resident]" : "") << "\n";
  }
  o << "endpoints:\n";
  for (const auto& e : endpoints_) {
    o << "  " << e.endpoint.kind << " " << e.endpoint.base << " (" << e.endpoint.label << "): "
      << (e.reachable ? "up, " + std::to_string(e.models) + " model(s)" : "unreachable — " + e.detail) << "\n";
  }
  return o.str();
}

std::string resolve_model_endpoint(const std::string& model, std::string* note) {
  static aios::hal::Mutex mu;
  static ModelController mc;
  static bool discovered = false;
  std::lock_guard<aios::hal::Mutex> g(mu);
  if (!discovered) { mc.discover(3000); discovered = true; }
  std::string where;
  std::string url = mc.endpoint_for(model, &where);
  if (url.empty()) {
    // One retry: the inventory may predate the model being pulled.
    mc.discover(3000);
    url = mc.endpoint_for(model, &where);
  }
  if (note) {
    if (url.empty()) *note = "no reachable endpoint lists a model named '" + model + "'";
    else *note = model + " is served by " + where + " (" + url + ")";
  }
  return url;
}

}  // namespace aios::cli
