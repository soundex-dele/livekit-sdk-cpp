#pragma once
#include "livekit/rtc/audio_output.hpp"
#include <functional>
#include <span>

namespace livekit::sdk::rtc::detail {
class AudioOutputDriver {
 public:
  // Serialized 10 ms pulls, 48 kHz stereo. No callbacks may survive stop().
  using Render = std::function<void(std::span<int16_t>)>;
  using Failure = std::function<void(std::string)>;
  virtual ~AudioOutputDriver() = default;
  virtual void start(const AudioOutputOptions&,Render,Failure) = 0;
  virtual void stop() noexcept = 0;
};
std::unique_ptr<AudioOutputDriver> create_native_audio_output_driver();
struct AudioOutputFactory {
  static std::shared_ptr<AudioOutputSession> create(AudioOutputOptions options,std::unique_ptr<AudioOutputDriver> driver) {
    return std::shared_ptr<AudioOutputSession>(new AudioOutputSession(options,std::move(driver)));
  }
};
}
