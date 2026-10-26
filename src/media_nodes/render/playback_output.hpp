#pragma once
#include "livekit/player.hpp"
#include "media_graph/channel.hpp"
#include "media_graph/media_flow.hpp"
#include <mutex>

namespace livekit::sdk::detail {
// Lifecycle and media subscriptions run on the graph worker. Channel access is
// synchronized so the application's render/device thread can pull frames.
template<class T> class PlaybackOutputNode final : public MediaNode {
 public:
  PlaybackOutputNode(const char* name, graphflow::core::EventBus* bus, size_t capacity, size_t bytes)
      : MediaNode(name, bus), channel_(capacity, bytes) {}
  int64_t end_us() const { return end_; } // playback worker only
  bool push(T& frame) {
    std::lock_guard lock(mutex_);
    if (released_) throw PlayerError("playback/output: node is released");
    frame.generation = generation_;
    if constexpr (std::is_same_v<T, media::VideoFrame>) return channel_.push(frame, frame.rgba->size());
    else return channel_.push(frame, frame.pcm->size() * sizeof(float));
  }
  std::optional<T> poll(int64_t time, uint64_t generation) {
    std::lock_guard lock(mutex_);
    return channel_.pop_due(time, generation);
  }
  void flush(uint64_t generation) {
    std::lock_guard lock(mutex_);
    channel_.clear(); generation_ = generation;
  }
  void clear() { std::lock_guard lock(mutex_); channel_.clear(); }
  size_t size() const { std::lock_guard lock(mutex_); return channel_.size(); }
 protected:
  bool doInit() override {
    using E = std::conditional_t<std::is_same_v<T, media::VideoFrame>, RGBA8, StereoFloat48k>;
    subscribe_media<E>([this](E& event) {
      if (event.eos) { finished_ = true; return true; }
      const auto end = event.frame.timestamp_us + event.frame.duration_us;
      if (!push(event.frame)) return false;
      end_ = std::max(end_, end); return true;
    });
    return true;
  }
  void on_reset(int64_t position) override { clear(); end_ = position; }
  void doRelease() override {
    std::lock_guard lock(mutex_); channel_.clear(); released_ = true;
  }
 private:
  mutable std::mutex mutex_;
  MediaChannel<T> channel_;
  uint64_t generation_ = 0;
  bool released_ = false;
  int64_t end_ = 0;
};
using VideoFrameOutputNode = PlaybackOutputNode<media::VideoFrame>;
using AudioFrameOutputNode = PlaybackOutputNode<media::AudioFrame>;
struct PlaybackOutputs {
  PlaybackOutputs(graphflow::core::EventBus* bus, const PlayerOptions& options)
      : video(std::make_shared<VideoFrameOutputNode>("video.output", bus, options.video_queue_frames, options.queue_bytes)),
        audio(std::make_shared<AudioFrameOutputNode>("audio.output", bus, options.audio_queue_frames, options.queue_bytes)) {}
  void flush(uint64_t generation) { video->flush(generation); audio->flush(generation); }
  void clear() { video->clear(); audio->clear(); }
  bool empty() const { return !video->size() && !audio->size(); }
  std::shared_ptr<VideoFrameOutputNode> video;
  std::shared_ptr<AudioFrameOutputNode> audio;
};
}
