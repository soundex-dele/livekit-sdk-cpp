#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>

namespace livekit::sdk::rtc {
class AudioRenderReference;

struct AudioOutputOptions {
  // Unset selects the system default playback device (Windows eConsole),
  // rather than the separate default communications endpoint.
  std::optional<uint16_t> device_index;
  size_t max_tracks = 32;
  std::shared_ptr<AudioRenderReference> echo_reference;
  std::chrono::milliseconds buffer{40}, max_latency{250};
};
}  // namespace livekit::sdk::rtc
