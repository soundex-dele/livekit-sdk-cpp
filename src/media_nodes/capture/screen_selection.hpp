#pragma once
#include "livekit/media.hpp"
#include <algorithm>

namespace lk {
inline livekit::sdk::Result<livekit::sdk::ScreenCaptureDevice> select_screen(
    const std::vector<livekit::sdk::ScreenCaptureDevice>& devices, const std::string& id) {
  using namespace livekit::sdk;
  auto it = std::find_if(devices.begin(), devices.end(),
                         [&](const auto& d) { return id.empty() ? d.primary : d.id == id; });
  if (it == devices.end()) {
    return Result<ScreenCaptureDevice>::failure(
        make_failure(ErrorDomain::media, ErrorCode::request_failed,
                     "Selected display unavailable; select a display again"));
  }
  return Result<ScreenCaptureDevice>::success(*it);
}

inline livekit::sdk::Result<void> validate_screen(
    const livekit::sdk::ScreenCaptureDevice& selected,
    const std::vector<livekit::sdk::ScreenCaptureDevice>& devices) {
  using namespace livekit::sdk;
  auto current = select_screen(devices, selected.id);
  if (!current) {
    return Result<void>::failure(current.failure_record());
  }
  if (*current != selected) {
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::request_failed,
                                              "Display mode changed; select a display again"));
  }
  return Result<void>::success();
}
}  // namespace lk
