#pragma once
#include "session.hpp"

namespace demo {
// Convenience authentication for a trusted local test server.
sdk::Result<SessionOptions> username_options(std::string url, std::string room,
                                             std::string username);
}  // namespace demo
