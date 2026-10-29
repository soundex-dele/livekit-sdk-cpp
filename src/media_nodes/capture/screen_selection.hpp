#pragma once
#include <livekit/media.hpp>
#include <algorithm>
#include <stdexcept>

namespace lk {
inline livekit::sdk::ScreenCaptureDevice select_screen(
    const std::vector<livekit::sdk::ScreenCaptureDevice>& devices, const std::string& id) {
  auto it = std::find_if(devices.begin(), devices.end(),
                         [&](const auto& d) { return id.empty() ? d.primary : d.id == id; });
  if (it == devices.end())
    throw std::runtime_error("Selected display unavailable; select a display again");
  return *it;
}

inline void validate_screen(const livekit::sdk::ScreenCaptureDevice& selected,
                            const std::vector<livekit::sdk::ScreenCaptureDevice>& devices) {
  if (select_screen(devices, selected.id) != selected)
    throw std::runtime_error("Display mode changed; select a display again");
}
}  // namespace lk
