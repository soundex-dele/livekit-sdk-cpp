#pragma once
#include "livekit/media.hpp"
#include "engines/rtc/rtc_graph.hpp"
#include "api/video/video_frame.h"
#include "media_nodes/capture/video_source.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include "media_nodes/capture/encoded_source.hpp"
#include <atomic>
#include <mutex>
#include <thread>

namespace lk {
class Process;
using livekit::sdk::MediaSpec;
class CaptureNode final : public livekit::sdk::detail::MediaNode {
 public:
  explicit CaptureNode(MediaSpec spec);
  ~CaptureNode();
  void activate(); // Start production only after every downstream node is ready.
  std::shared_ptr<livekit::sdk::detail::EncodedSourceReader> encoded_source() const{return external_encoded_;}
  bool finished() const { return finished_; }
  std::string error() const;
  const MediaSpec spec;
  std::atomic<uint64_t> frames{0};
 protected:
  bool doStart() override;
  void doStop() override;
 private:
  void deliver(const webrtc::VideoFrame& frame);
  void run();
  std::thread thread_;
  std::unique_ptr<livekit::sdk::detail::I420SourceReader> external_video_;
  std::unique_ptr<livekit::sdk::detail::PcmSourceReader> external_audio_;
  std::shared_ptr<livekit::sdk::detail::EncodedSourceReader> external_encoded_;
  std::stop_source external_stop_;
  std::atomic<bool> stop_{false}, finished_{false};
  mutable std::mutex mutex_;
  std::shared_ptr<Process> process_;
  std::string error_;
};
}
