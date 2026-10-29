#pragma once
#include "media_nodes/playout/audio_output_driver.hpp"
#include "api/audio/audio_device_defines.h"
#include <memory>

namespace livekit::sdk::rtc::detail {
// Testable callback boundary. Owns no ADM and opens no physical device.
class DeviceOutputTransport : public webrtc::AudioTransport {
 public:
  ~DeviceOutputTransport() override = default;
};

std::unique_ptr<DeviceOutputTransport> create_audio_output_transport(AudioOutputDriver::Render,
                                                                     AudioOutputDriver::Failure);
}  // namespace livekit::sdk::rtc::detail
