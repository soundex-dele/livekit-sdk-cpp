#pragma once
#include <livekit/participant.hpp>
#include <chrono>
#include <memory>
#include <string>

namespace livekit::sdk {
inline constexpr const char* remote_control_topic = "av.remote-control.v1";
using ControlClock = std::chrono::steady_clock;
struct ControlLayout {
  uint64_t version = 1;
  int x = 0, y = 0, width = 0, height = 0;
  bool operator==(const ControlLayout&) const = default;
};
struct ControlGrant {
  std::string id, controller_identity;
  uint64_t epoch = 0;
  ControlClock::time_point deadline;
};
struct ControlInput {
  std::string type, code;
  bool down = false;
  bool positioned = false;
  int x = 0, y = 0, button = 0, delta = 0;
};
class ControlInputSink {
 public:
  virtual ~ControlInputSink() = default;
  virtual bool apply(const ControlInput&) noexcept = 0;
};
// Windows ordinary interactive desktop only. These functions never elevate.
std::shared_ptr<ControlInputSink> make_windows_control_input();
ControlLayout primary_control_layout();
// Portable browser KeyboardEvent.code -> PC set-1 scan code (bit 8 = extended).
unsigned control_key_scan_code(const std::string& code);

class RemoteControlSession {
 public:
  explicit RemoteControlSession(std::shared_ptr<ControlInputSink>, bool watchdog = true);
  ~RemoteControlSession();
  RemoteControlSession(const RemoteControlSession&) = delete;
  RemoteControlSession& operator=(const RemoteControlSession&) = delete;
  // Trusted application supplies an ONLINE verified business grant; never call
  // this from a data-message handler. Deadlines are monotonic and include HTTP RTT.
  void authorize(ControlGrant, ControlLayout, ControlClock::time_point now = ControlClock::now());
  bool renew(const ControlGrant&, ControlClock::time_point now = ControlClock::now());
  void set_layout(ControlLayout);
  bool receive(const std::vector<uint8_t>&, const DataReceiveParams&, ControlClock::time_point now = ControlClock::now());
  void tick(ControlClock::time_point now = ControlClock::now());
  void stop(const std::string& reason = "stopped") noexcept;
  bool active() const;
  std::string reason() const;
  Json status() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
