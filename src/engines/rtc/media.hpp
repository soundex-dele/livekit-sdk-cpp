#pragma once
#include "server_sdk/auth.hpp"
#include "livekit/media.hpp"
#include "support/process.hpp"
#include "api/peer_connection_interface.h"
#include "media/base/adapted_video_track_source.h"
#include "engines/rtc/rtc_graph.hpp"
#include "media_nodes/render/video_render.hpp"
#include "media_nodes/capture/capture_source.hpp"
#include "media_nodes/recording/recording.hpp"
#include "backends/webrtc/graph_video_encoder.hpp"
#include "engines/rtc/video_receive_graph.hpp"
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
  explicit VideoSource(bool screen = false) : screen_(screen) {}
  SourceState state() const override { return kLive; }
  bool remote() const override { return false; }
  bool is_screencast() const override { return screen_; }
  std::optional<bool> needs_denoising() const override { return false; }
  void push(const webrtc::VideoFrame& frame) { OnFrame(frame); }
 private:
  bool screen_;
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
// Track-level owner; GraphEngine owns and connects the actual processing nodes.
class MediaPump {
 private:
  std::shared_ptr<CaptureNode> capture_;
  std::unique_ptr<livekit::sdk::detail::GraphVideoSender> sender_;
  std::unique_ptr<livekit::sdk::detail::RtcPipeline> pipeline_;
 public:
  explicit MediaPump(MediaSpec spec, std::shared_ptr<livekit::sdk::detail::RtcGraph> graph = {},
                    VideoPlayout* playout = nullptr, VideoFrames* frames = nullptr, std::string id = {});
  ~MediaPump();
  bool start();
  void stop();
  bool finished() const { return capture_->finished(); }
  std::string error() const { return capture_->error(); }
  const MediaSpec& spec;
  std::atomic<uint64_t>& frames;
  webrtc::scoped_refptr<VideoSource> video;
  webrtc::scoped_refptr<AudioSource> audio;
};
class TrackSink : public webrtc::AudioTrackSinkInterface {
 public:
  TrackSink(webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track, const std::filesystem::path& directory, VideoPlayout* playout = nullptr, VideoFrames* frames = nullptr, std::string track_id = {}, std::shared_ptr<livekit::sdk::detail::RtcGraph> graph = {}, std::vector<uint32_t> ssrcs = {});
  ~TrackSink();
  void attach();
  void detach();
  void OnData(const void* data, int bits, int rate, size_t channels, size_t frames) override;
  std::string error() const;
  Json stats() const;
  std::string track_id() const { return track_id_; }
  bool owns_track(const webrtc::MediaStreamTrackInterface* track) const { return track_.get() == track; }
 private:
  webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track_;
  std::string track_id_;
  VideoPlayout* playout_ = nullptr;
  VideoFrames* frame_output_ = nullptr;
  mutable std::mutex mutex_;
  uint64_t frames_ = 0, bytes_ = 0;
  bool attached_ = false;
  std::string error_;
  std::shared_ptr<RecordingNode> recorder_;
  std::shared_ptr<livekit::sdk::detail::MediaEventNode<livekit::sdk::detail::PcmFrame>> audio_input_;
  std::shared_ptr<livekit::sdk::detail::MediaNode> subscribe_node_;
  std::unique_ptr<livekit::sdk::detail::RtcPipeline> pipeline_;
  std::shared_ptr<livekit::sdk::detail::RtcGraph> registry_;
  std::shared_ptr<livekit::sdk::detail::VideoReceiveGraph> receive_video_;
  std::vector<uint32_t> ssrcs_;
};
}
