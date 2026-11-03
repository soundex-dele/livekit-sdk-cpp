#pragma once
#include "livekit/media_engine.hpp"
#include "livekit/rtc/input_node.hpp"
#include "livekit/rtc/audio_output.hpp"
#include "livekit/rtc/recording_node.hpp"
#include "engines/media/application_output.hpp"
#include "engines/media/sync_node.hpp"
#include "engine/graph_engine.h"

namespace livekit::sdk::client::detail {
class MediaPort;

class MediaPipeline : public std::enable_shared_from_this<MediaPipeline> {
 public:
  ~MediaPipeline();
  Result<void> prepare(const MediaPublishSpec&, const LocalTrack&,
                       std::shared_ptr<rtc::AudioOutputSession>);
  Result<void> prepare(const MediaSubscribeSpec&, const RemoteTrack&, rtc::TrackDescriptor,
                       std::shared_ptr<rtc::AudioOutputSession>,
                       std::shared_ptr<rtc::detail::MediaSyncClock>);
  Result<void> start();
  void close();
  MediaTrackSnapshot snapshot() const;
  LocalTrack local;
  RemoteTrack remote;
  LocalTrackPublication publication;
  RemoteTrackPublication subscription;

 private:
  template <class Event, class From, class To>
  void connect(const From& from, const To& to) {
    edges_.push_back(
        {from->getName(), to->getName(), graphflow::core::detail::prettyName<Event>(), {}});
  }

  void outputs(const std::shared_ptr<graphflow::core::GraphNode>&, MediaOutput,
               const rtc::TrackDescriptor&, std::shared_ptr<rtc::AudioOutputSession>);
  Result<void> build();
  void stop_nodes();
  void fail(Failure);
  mutable std::mutex state_mutex_, lifecycle_;
  Failure failure_;
  bool publishing_ = false, stopped_ = false;
  std::atomic<bool> stopping_{false};
  std::thread pump_;
  std::unique_ptr<graphflow::engine::GraphEngine> graph_;
  std::vector<std::shared_ptr<graphflow::core::GraphNode>> nodes_;
  std::vector<graphflow::engine::NodeConnection> edges_;
  std::shared_ptr<MediaPort> port_;
  std::shared_ptr<rtc::nodes::MediaInputNode> input_;
  std::shared_ptr<rtc::nodes::VideoCodecNode> video_;
  std::shared_ptr<rtc::nodes::AudioCodecNode> audio_;
  std::shared_ptr<rtc::engine::ApplicationOutput> output_;
  std::shared_ptr<rtc::nodes::AudioOutputNode> speaker_;
  std::shared_ptr<rtc::nodes::RecordingNode> recording_;
};
}  // namespace livekit::sdk::client::detail
