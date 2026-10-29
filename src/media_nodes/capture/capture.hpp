#pragma once
#include "livekit/media.hpp"
#include "api/video/video_frame.h"
#include <atomic>
#include <functional>

namespace lk {
// Blocking source loop owned by the capture node. Stop joins all device
// callbacks before the source or its downstream leases can be destroyed.
void capture_device(const livekit::sdk::MediaSpec& spec, std::atomic<bool>& stop,
                    const std::function<void(const webrtc::VideoFrame&)>& output);
}  // namespace lk
