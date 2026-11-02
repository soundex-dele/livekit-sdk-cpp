#include "utoolkit/logging/logger.h"

#include <cstdio>
#include <exception>
#include <mutex>

namespace livekit::sdk::client::detail {
// Exception-enabled boundary around the existing third-party logging backend.
// Client/session code remains compiled without exceptions.
void initialize_logging() noexcept {
  static std::once_flag once;
  std::call_once(once, [] {
    try {
      utoolkit::logging::Logger::GetInstance().Initialize("livekit-client");
    } catch (const std::exception& failure) {
      std::fprintf(stderr, "LiveKit logging initialization failed: %s\n", failure.what());
    } catch (...) {
      std::fputs("LiveKit logging initialization failed\n", stderr);
    }
  });
}
}  // namespace livekit::sdk::client::detail
