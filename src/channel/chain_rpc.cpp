#include "aios/channel/chain_rpc.hpp"

namespace aios::channel {

Result<void> ChainRpc::open(const std::string& url, int timeout_ms, hal::Host* host,
                            const std::string& kernel_root) {
  auto ep = parse_endpoint(url);
  if (!ep.ok()) return Result<void>(ep.error());
  endpoint_ = *ep;
  timeout_ms_ = timeout_ms;
  host_ = host;
  kernel_root_ = kernel_root;
  ready_ = true;
  description_ = endpoint_.host + ":" + std::to_string(endpoint_.port) + endpoint_.path;
  return Ok();
}

Result<RpcValue> ChainRpc::call(const std::string& method, const std::vector<RpcValue>& params) {
  if (!ready_) return Result<RpcValue>(Status::Invalid, "no node configured");

  std::string document = rpc_request(method, params, next_id_++);

  HttpRequest request;
  request.method = "POST";
  request.endpoint = endpoint_;
  request.timeout_ms = timeout_ms_;
  request.host = host_;
  request.kernel_root = kernel_root_;
  request.headers.emplace_back("Content-Type", "application/json");
  request.headers.emplace_back("Accept", "application/json");
  request.body.assign(document.begin(), document.end());

  auto response = http_send(request);
  if (!response.ok()) return Result<RpcValue>(response.error());

  auto parsed = rpc_parse(ByteSpan(response->body.data(), response->body.size()));
  if (!parsed.ok()) {
    if (response->status != 200) {
      return Result<RpcValue>(Status::IoError,
                              method + ": node answered " + std::to_string(response->status) + " " +
                                  response->reason);
    }
    return Result<RpcValue>(parsed.error());
  }

  const RpcValue& error = (*parsed)["error"];
  if (!error.is_null()) {
    std::string message = error["message"].as_text();
    if (message.empty()) message = error.render();
    return Result<RpcValue>(Status::Invalid, method + ": " + message);
  }

  if (response->status != 200) {
    return Result<RpcValue>(Status::IoError,
                            method + ": node answered " + std::to_string(response->status));
  }

  return (*parsed)["result"];
}

}  // namespace aios::channel
