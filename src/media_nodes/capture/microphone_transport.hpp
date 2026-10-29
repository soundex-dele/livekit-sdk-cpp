#pragma once
#include "media_nodes/capture/microphone_driver.hpp"
#include "api/audio/audio_device_defines.h"

namespace livekit::sdk::detail {
class DeviceMicrophoneTransport : public webrtc::AudioTransport {
 public:
  ~DeviceMicrophoneTransport() override = default;
};

// Testable ADM callback boundary; no physical device is created.
std::unique_ptr<DeviceMicrophoneTransport> create_microphone_transport(MicrophoneDriver::Callback);
}  // namespace livekit::sdk::detail
