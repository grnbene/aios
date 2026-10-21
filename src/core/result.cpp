#include "aios/core/result.hpp"

namespace aios {
const char* status_name(Status s) {
  switch (s) {
    case Status::Ok: return "ok";
    case Status::NotFound: return "not-found";
    case Status::Denied: return "denied";
    case Status::Exhausted: return "exhausted";
    case Status::Invalid: return "invalid";
    case Status::Conflict: return "conflict";
    case Status::Unsupported: return "unsupported";
    case Status::IoError: return "io-error";
    case Status::Corrupt: return "corrupt";
    case Status::Busy: return "busy";
    case Status::Expired: return "expired";
  }
  return "?";
}
}  // namespace aios
