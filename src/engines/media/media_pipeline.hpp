#pragma once
#include "livekit/media_engine.hpp"
#include "livekit/media_graph.hpp"
#include "livekit/rtc/input_node.hpp"
#include "livekit/rtc/audio_output.hpp"
#include "livekit/rtc/recording_node.hpp"
#include "engines/media/application_output.hpp"
#include "engines/media/sync_node.hpp"

#include <condition_variable>
#include <future>

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
    edges_.push_back(rtc::TrackConnection::between<Event>(from->name(), to->name()));
  }

  void outputs(const std::shared_ptr<livekit::sdk::rtc::TrackNode>&, MediaOutput,
               const rtc::TrackDescriptor&, std::shared_ptr<rtc::AudioOutputSession>);
  Result<void> build(const std::shared_ptr<const MediaGraphOptions>&, const rtc::TrackDescriptor&);
  Result<void> initialize_on_owner();
  Result<void> activate_on_owner();
  void pump_on_owner();
  void stop_nodes();
  void fail(Failure);
  mutable std::mutex state_mutex_;
  std::mutex control_mutex_, close_mutex_;
  std::condition_variable control_changed_;
  std::promise<Result<void>> activation_;
  bool start_requested_ = false;
  Failure failure_;
  bool publishing_ = false, stopped_ = false;
  std::atomic<bool> stopping_{false};
  std::thread pump_;
  std::unique_ptr<graphflow::Graph> graph_;
  std::vector<std::shared_ptr<livekit::sdk::rtc::TrackNode>> nodes_;
  std::vector<rtc::TrackConnection> edges_;
  std::shared_ptr<MediaPort> port_;
  std::shared_ptr<rtc::nodes::MediaInputNode> input_;
  std::shared_ptr<rtc::nodes::VideoCodecNode> video_;
  std::shared_ptr<rtc::nodes::AudioCodecNode> audio_;
  std::shared_ptr<rtc::engine::ApplicationOutput> output_;
  std::shared_ptr<rtc::nodes::AudioOutputNode> speaker_;
  std::shared_ptr<rtc::nodes::RecordingNode> recording_;
  std::function<bool()> custom_activate_;
  std::function<void()> custom_quiesce_;
};
}  // namespace livekit::sdk::client::detail
