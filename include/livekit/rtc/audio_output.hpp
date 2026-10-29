#pragma once
#include "livekit/rtc/audio_nodes.hpp"

namespace livekit::sdk::rtc {
namespace detail { class AudioOutputDriver; struct AudioOutputFactory; }
namespace nodes { class AudioOutputNode; }
struct AudioOutputOptions {
  std::optional<uint16_t> device_index;
  size_t max_tracks = 32;
  std::chrono::milliseconds buffer{40}, max_latency{250};
};
struct AudioOutputSnapshot {
  size_t tracks = 0;
  uint64_t rendered_blocks = 0, contention_blocks = 0;
  bool running = false;
  std::string error;
};
struct AudioOutputTrackSnapshot {
  size_t queued_samples = 0; // Per-channel samples, 48 kHz.
  uint64_t admitted = 0, dropped = 0, rendered_samples = 0, silence_samples = 0, resets = 0;
  Epoch epoch = 0;
  bool closed = false, muted = false;
};
class AudioOutputTrack;
// Shared physical device and bounded per-track PCM mixer above RTC. Starts on
// the first track and stops after the last closes. Construction opens no device.
// Track RTP clocks are anchored independently to the local device clock; this
// does not provide sender A/V synchronization, NetEq, PLC, AEC or adaptive jitter.
class AudioOutputSession final : public std::enable_shared_from_this<AudioOutputSession> {
 public:
  explicit AudioOutputSession(AudioOutputOptions = {});
  ~AudioOutputSession();
  AudioOutputSession(const AudioOutputSession&) = delete;
  AudioOutputSession& operator=(const AudioOutputSession&) = delete;
  // Construct the session with shared ownership before opening tracks.
  std::shared_ptr<AudioOutputTrack> open_track(TrackId);
  AudioOutputSnapshot snapshot() const;
  void throw_if_failed() const;
 private:
  friend class AudioOutputTrack;
  friend struct detail::AudioOutputFactory;
  AudioOutputSession(AudioOutputOptions,std::unique_ptr<detail::AudioOutputDriver>);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class AudioOutputTrack final {
 public:
  ~AudioOutputTrack();
  AudioOutputTrack(const AudioOutputTrack&) = delete;
  AudioOutputTrack& operator=(const AudioOutputTrack&) = delete;
  // PCM16 at 48 kHz, mono/stereo, 2.5..120 ms. Does not wait for device space.
  // Gaps produce silence. Late/overlapping frames are dropped. Overflow or a
  // timestamp jump beyond max_latency flushes this track and reanchors it.
  bool write(const nodes::AudioFrameEvent&);
  void set_gain(float); // 0..4; saturation happens after mixing all tracks.
  void set_muted(bool); // Clears only this track's queue on each transition.
  void close(); // Fences this track out of subsequent render callbacks.
  AudioOutputTrackSnapshot snapshot() const;
 private:
  friend class AudioOutputSession;
  struct State;
  AudioOutputTrack(std::shared_ptr<AudioOutputSession>,std::shared_ptr<State>);
  std::shared_ptr<AudioOutputSession> session_;
  std::shared_ptr<State> state_;
};
namespace nodes {
// One subscription sink. Many sinks can share one AudioOutputSession.
class AudioOutputNode final : public graphflow::core::GraphNode {
 public:
  AudioOutputNode(std::string name,TrackId,std::shared_ptr<AudioOutputSession>);
  ~AudioOutputNode() override;
  void quiesce();
  void set_gain(float);
  void set_muted(bool);
  AudioOutputTrackSnapshot snapshot() const;
  void throw_if_failed() const;
 protected:
  bool doInit() override;
  bool doStart() override;
  void doStop() override;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
}
