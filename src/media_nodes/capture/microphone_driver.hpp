#pragma once
#include "livekit/microphone.hpp"
#include <functional>
#include <span>

namespace livekit::sdk::detail {
struct MicrophoneBlock {
  std::span<const int16_t> samples;
  int sample_rate = 48000;
  size_t channels = 1;
  std::chrono::steady_clock::time_point captured;
};
class MicrophoneDriver {
 public:
  using Callback = std::function<void(const MicrophoneBlock&)>;
  virtual ~MicrophoneDriver() = default;
  // Callbacks are serialized, may begin inside start(), and must not escape stop().
  // stop() must also clean up a partially failed start().
  virtual void start(const MicrophoneOptions&, Callback) = 0;
  virtual void stop() noexcept = 0;
};
std::unique_ptr<MicrophoneDriver> create_native_microphone_driver();
struct MicrophoneFactory {
  static std::shared_ptr<MicrophoneSession> create(MicrophoneOptions options, std::unique_ptr<MicrophoneDriver> driver) {
    return std::shared_ptr<MicrophoneSession>(new MicrophoneSession(options,std::move(driver)));
  }
};
}
