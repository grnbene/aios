#pragma once

namespace aios::userland::chain {

// Where value is directed. These are Sean's accounts, deliberately compiled in
// and deliberately public: an address is a destination, not a secret, and
// anyone reading them can only send to them.
//
// AIOS holds no private key for either. They are receive-only. Nothing in this
// tree can spend from them, and a balance shown against them is somebody else's
// money that this machine can watch and not touch — which is exactly the
// distinction the wallet used to blur when it printed them as its own.
inline constexpr const char* PAYOUT_ETH = "0xD6c833c7C424344Daf6F49342A11c08f3CE82c96";
inline constexpr const char* PAYOUT_BTC = "35grv9BWGSHkQyGCk5uKfXj2wNWLpMXbsh";
inline constexpr const char* PAYOUT_XMR = "44AFFq5AxmPDMsYeMwQvK956Jx5GmFi2KUZBRAtihChbgPQT576xAZFcETZQuWNWAnRFqh9Y78GmpsTX5wDZn9t67Su787R";

}  // namespace aios::userland::chain
