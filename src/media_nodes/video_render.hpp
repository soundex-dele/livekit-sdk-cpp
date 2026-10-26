#pragma once
#include "media_graph/video_events.hpp"
#include "lk/video_frames.hpp"
#include "lk/video_playout.hpp"
namespace lk {
// Independent render/output node: the targets own their bounded latest-frame
// mailboxes and execute rendering / application callbacks on their owner thread.
class VideoRenderNode final : public livekit::sdk::detail::MediaNode {
 public:
  VideoRenderNode(std::string id, VideoPlayout* playout, VideoFrames* frames)
      : MediaNode("VideoRenderNode", livekit::sdk::detail::NodeExecution::sink),
        id_(std::move(id)), playout_(playout), frames_(frames) {}
  ~VideoRenderNode() override { release(); }
  void submit(const webrtc::VideoFrame& frame) {
    ++inputs;
    if (playout_) playout_->submit(id_, frame);
    if (frames_) frames_->submit(id_, frame);
    ++outputs;
  }
 protected:
  bool doInit() override {
    subscribe<livekit::sdk::detail::RawVideoFrame>([this](auto& event) { submit(event.frame); });
    return true;
  }
  bool doStart() override {
    if (playout_) playout_->add_track(id_);
    if (frames_) frames_->add(id_);
    return true;
  }
  void doStop() override {
    if (playout_) playout_->remove_track(id_);
    if (frames_) frames_->remove(id_);
  }
 private:
  std::string id_;
  VideoPlayout* playout_;
  VideoFrames* frames_;
};
}
