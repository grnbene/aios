#pragma once
#include <string>
#include <vector>
#include "aios/channel/http_client.hpp"
#include "aios/channel/rpc_value.hpp"
#include "aios/core/result.hpp"

namespace aios::channel {

// A remote procedure channel to a node we run. Both bitcoind and every Ethereum
// execution client speak the same envelope over the same transport; only the
// method names differ, so they differ nowhere below this line.
class ChainRpc {
 public:
  ChainRpc() = default;
  Result<void> open(const std::string& url, int timeout_ms = 20000, hal::Host* host = nullptr,
                    const std::string& kernel_root = "");
  bool ready() const { return ready_; }
  const std::string& describe() const { return description_; }

  Result<RpcValue> call(const std::string& method, const std::vector<RpcValue>& params = {});

 private:
  HttpEndpoint endpoint_;
  int timeout_ms_ = 20000;
  uint64_t next_id_ = 1;
  bool ready_ = false;
  std::string description_;
  hal::Host* host_ = nullptr;
  std::string kernel_root_;
};

}  // namespace aios::channel
