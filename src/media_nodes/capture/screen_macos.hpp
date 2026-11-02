#pragma once
#include "media_nodes/capture/capture.hpp"

namespace lk {
livekit::sdk::Result<std::vector<livekit::sdk::ScreenCaptureDevice>> mac_screen_devices();
livekit::sdk::Result<void> capture_screen_macos(
    const livekit::sdk::MediaSpec& spec, std::atomic<bool>& stop,
    const std::function<livekit::sdk::Result<void>(const webrtc::VideoFrame&)>& output);
}  // namespace lk
