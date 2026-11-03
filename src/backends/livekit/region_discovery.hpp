#pragma once
#include "backends/livekit/rtc_session.hpp"

namespace livekit::sdk::client::detail {
std::vector<std::string> discover_regions(const ConnectionOptions&, const Operation&);
}
