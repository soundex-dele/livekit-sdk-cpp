#pragma once
#include <livekit/remote_control.hpp>
#include <optional>
#include <set>

namespace livekit::sdk::detail {
// Portable native boundary; tests record events without invoking CoreGraphics.
struct MacInputEvent {
  std::string type;
  int key = 0, button = 0, delta = 0;
  bool down = false;
  double x = 0, y = 0;
  uint64_t flags = 0;
  std::u16string text;
};

struct MacDisplayGeometry {
  int width = 0, height = 0;
  double x = 0, y = 0, logical_width = 0, logical_height = 0;
};

class MacInputApi {
 public:
  virtual ~MacInputApi() = default;
  virtual bool available() noexcept = 0;
  virtual MacDisplayGeometry display() noexcept = 0;
  virtual bool send(const MacInputEvent&) noexcept = 0;
};

std::optional<int> mac_key_code(const std::string& code);
std::shared_ptr<ControlInputSink> mac_input_with_api(std::shared_ptr<MacInputApi> api);
}  // namespace livekit::sdk::detail
