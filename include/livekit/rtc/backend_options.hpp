#pragma once
#include <string>

namespace livekit::sdk::rtc {
// Backend-specific immutable connection configuration. RoomEngine owns only the
// session/track state machine; the selected adapter validates this configuration.
class BackendOptions {
 public:
  virtual ~BackendOptions() = default;
  virtual std::string backend_id() const = 0;
};

struct BackendCapabilities {
  std::string backend;
  bool participant_update_ack = false;
  bool subscription_permissions = false;
  bool receive_demand = false;
  bool room_services = false;
  bool media_encryption = false;
  bool full_rejoin = false;
  bool signaling_resume = false;
  bool simulcast = false;
};
}  // namespace livekit::sdk::rtc
