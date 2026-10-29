#pragma once
#include "media_nodes/capture/capture.hpp"

namespace lk {
std::vector<livekit::sdk::ScreenCaptureDevice> mac_screen_devices();
void capture_screen_macos(const livekit::sdk::MediaSpec& spec, std::atomic<bool>& stop,
                          const std::function<void(const webrtc::VideoFrame&)>& output);
}  // namespace lk
