#pragma once
#include "engines/rtc/command.hpp"
#include <limits>
#include <map>

namespace livekit::sdk::detail {
// Runner-owned request correlation. Command itself arbitrates asynchronous
// cancellation against response completion; IDs are never reused in a transport.
class PendingRequests {
 public:
  uint32_t add(const std::shared_ptr<Command>& command) {
    poll();
    if (requests_.size() >= 1024 || next_ == std::numeric_limits<uint32_t>::max())
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::resource_exhausted, "Pending request capacity exceeded");
    const auto id = ++next_;
    requests_.emplace(id, command); return id;
  }
  void respond(uint32_t id, int reason) {
    const auto found = requests_.find(id);
    if (found == requests_.end()) return; // duplicate/late completion
    auto command = found->second;
    command->expire(std::chrono::steady_clock::now());
    if (reason == 4 && !command->done()) return; // QUEUED is not final confirmation
    requests_.erase(found);
    if (reason == 0) command->resolve();
    else {
      const auto code = reason == 2 ? ErrorCode::permission_denied : reason == 3 ? ErrorCode::resource_exhausted :
          reason == 5 ? ErrorCode::unsupported : ErrorCode::request_failed;
      command->reject(std::make_exception_ptr(SDKError(ErrorDomain::protocol, code,
          "Server rejected control request", false, reason)));
    }
  }
  void poll() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = requests_.begin(); it != requests_.end();) {
      it->second->expire(now);
      if (it->second->done()) it = requests_.erase(it); else ++it;
    }
  }
  void interrupt(const char* message) {
    auto requests = std::move(requests_); requests_.clear();
    for (auto& [id, command] : requests) command->interrupt(ErrorCode::session_closed, message);
  }
  size_t size() const { return requests_.size(); }
 private:
  uint32_t next_ = 0;
  std::map<uint32_t, std::shared_ptr<Command>> requests_;
};
}
