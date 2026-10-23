#pragma once
#include "lk/auth.hpp"
#include "livekit/media.hpp"
#include "lk/process.hpp"
#include "api/peer_connection_interface.h"
#include "media/base/adapted_video_track_source.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
namespace lk {
class VideoPlayout;
class VideoFrames;
class VideoSource : public webrtc::AdaptedVideoTrackSource {
 public:
  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return false; }
  std::optional<bool> needs_denoising() const override { return false; }
  void push(const webrtc::VideoFrame& frame) { OnFrame(frame); }
};
class AudioSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
 public:
  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  void AddSink(webrtc::AudioTrackSinkInterface* sink) override;
  void RemoveSink(webrtc::AudioTrackSinkInterface* sink) override;
  void push(const int16_t* samples);
 private:
  std::mutex mutex_;
  std::vector<webrtc::AudioTrackSinkInterface*> sinks_;
};
using livekit::sdk::MediaSpec;
std::vector<MediaSpec> probe_media(const std::string& input, const std::string& ffmpeg,
                                  bool loop, int width, int height, int fps);
class MediaPump {
 public:
  explicit MediaPump(MediaSpec spec);
  ~MediaPump();
  void start();
  void stop();
  bool finished() const { return finished_; }
  std::string error() const;
  const MediaSpec spec;
  webrtc::scoped_refptr<VideoSource> video;
  webrtc::scoped_refptr<AudioSource> audio;
  std::atomic<uint64_t> frames{0};
 private:
  void run();
  std::thread thread_;
  std::atomic<bool> stop_{false}, finished_{false};
  mutable std::mutex mutex_;
  std::shared_ptr<Process> process_;
  std::string error_;
};
class TrackSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame>, public webrtc::AudioTrackSinkInterface {
 public:
  TrackSink(webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const std::filesystem::path& directory, VideoPlayout* playout = nullptr, VideoFrames* frames = nullptr, std::string track_id = {});
  ~TrackSink();
  void attach();
  void detach();
  void OnFrame(const webrtc::VideoFrame& frame) override;
  void OnData(const void* data, int bits, int rate, size_t channels, size_t frames) override;
  std::string error() const;
  Json stats() const;
  std::string track_id() const { return track_id_; }
  bool owns_track(const webrtc::MediaStreamTrackInterface* track) const { return track_.get() == track; }
 private:
  void open_video(int width, int height);
  void close_audio();
  webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track_;
  std::string track_id_;
  VideoPlayout* playout_ = nullptr;
  VideoFrames* frame_output_ = nullptr;
  std::filesystem::path base_;
  std::ofstream file_;
  mutable std::mutex mutex_;
  uint64_t frames_ = 0, bytes_ = 0;
  int width_ = 0, height_ = 0, rate_ = 0, channels_ = 0, part_ = 0;
  bool attached_ = false;
  std::string error_;
};
}
