#pragma once
#include "livekit/audio_source.hpp"
#include "livekit/error.hpp"
#include <functional>

namespace livekit::sdk {
namespace rtc::nodes {
class MediaInputNode;
}
namespace detail {
class MicrophoneDriver;
struct MicrophoneFactory;
}  // namespace detail

struct MicrophoneOptions {
  // Unset selects the default communications device on Windows, device 0 elsewhere.
  std::optional<uint16_t> device_index;
  size_t channels = 1;
  std::chrono::milliseconds max_queue_duration{100};
};

struct MicrophoneSnapshot {
  bool running = false;
  uint64_t captured_blocks = 0, dropped_blocks = 0;
  std::string error;
  std::shared_ptr<const FailureInfo> failure;
};

// Physical capture above RTC. Produces bounded 48 kHz PCM16 blocks; owns no
// Room, PeerConnection, codec, APM or echo canceller. Construction opens no device.
// One MediaInputNode may consume source(); fan out downstream for multiple sinks.
class MicrophoneSession final {
 public:
  explicit MicrophoneSession(MicrophoneOptions = {});
  ~MicrophoneSession();
  MicrophoneSession(const MicrophoneSession&) = delete;
  MicrophoneSession& operator=(const MicrophoneSession&) = delete;
  std::shared_ptr<PcmAudioSource> source() const;
  // Attach/start the source reader before start(). Calls are serialized internally.
  // stop() fences device callbacks and permits a subsequent start().
  void start();
  void stop();
  MicrophoneSnapshot snapshot() const;
  void throw_if_failed() const;

 private:
  friend struct detail::MicrophoneFactory;
  friend class rtc::nodes::MediaInputNode;
  // The exclusive reader installs a retainable node failure reporter.
  void set_failure_handler(std::function<void(std::exception_ptr)>);
  MicrophoneSession(MicrophoneOptions, std::unique_ptr<detail::MicrophoneDriver>);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace livekit::sdk
