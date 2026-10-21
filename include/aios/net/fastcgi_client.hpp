#pragma once
// A FastCGI (RFC-less but universally-implemented mod_fastcgi wire protocol)
// client speaking the RESPONDER role to a PHP-FPM pool. The point of writing
// this instead of proxying HTTP to a second HTTP server is that FastCGI's
// STDIN/STDOUT are already framed as a byte stream of records — nothing here
// ever needs the whole request body or the whole response body in memory at
// once, which is the actual defect this replaces (nginx's default fastcgi
// buffering does hold a full response, and often a full request, before it
// moves on).
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aios/core/bytes.hpp"
#include "aios/core/result.hpp"
#include "aios/hal/net.hpp"

namespace aios::net {

using FcgiParams = std::vector<std::pair<std::string, std::string>>;

// Called with each slice of STDOUT (the raw CGI response: status/header lines
// then a blank line then body, exactly as PHP-FPM produced it — unparsed) as
// it arrives, so a caller can relay it onward without buffering the whole
// response either.
using FcgiOutputSink = std::function<void(ByteSpan chunk)>;

class FastCgiClient {
 public:
  FastCgiClient(std::string host, uint16_t port);

  // One RESPONDER request end to end:
  //  - opens the connection
  //  - sends BEGIN_REQUEST + PARAMS (params — SCRIPT_FILENAME, REQUEST_METHOD, etc.)
  //  - streams the body through STDIN by calling next_body_chunk() repeatedly
  //    until it returns an empty buffer (end of body); each call may block
  //    waiting on the real client, which is the point — this thread is a
  //    straight pipe, not a buffer.
  //  - streams STDOUT to `on_output` as records arrive
  // Returns once FCGI_END_REQUEST closes the exchange (or an error/timeout).
  struct Result_ {
    bool ok = false;
    std::string error;
    uint32_t app_status = 0;
  };
  // should_abort, when given, is asked after every body chunk: true drops the connection to php-fpm at once (which
  // stops the request) and returns with error set, so a body the caller refuses is not handed on as if it were whole.
  Result_ run(const FcgiParams& params, const std::function<ByteBuffer()>& next_body_chunk,
              const FcgiOutputSink& on_output, int timeout_ms = 60000, const std::function<bool()>& should_abort = {});

 private:
  std::string host_;
  uint16_t port_;
};

}  // namespace aios::net
