#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/media_config.hpp"
#include "livekit/rtc/stream_pipeline.hpp"
#include <memory>
#include <optional>
#include <string>

namespace livekit::sdk::rtc::engine {
// Single-room SDK facade. Control, poll and destruction use the creating thread.
// Media callbacks must be bounded and cannot reenter lifecycle operations.
// Futures for graph/media creation require poll(); join/leave use the room worker.
class RtcEngine final {
 public:
  explicit RtcEngine(RoomEngine::TransportFactory);
  explicit RtcEngine(std::unique_ptr<RoomEngine>);
  ~RtcEngine();
  RtcEngine(const RtcEngine&) = delete;
  RtcEngine& operator=(const RtcEngine&) = delete;

  // Links LiveKit::RTCMediaEngine. Call before join; repeated installation is rejected.
  void enable_media(AudioOutputOptions = {});
  bool media_enabled() const;
  std::future<void> join(RoomOptions);
  // Quiesces graphs before leaving. Await completion before joining again.
  std::future<void> leave();
  // Terminal and idempotent. Rejects subsequent control operations.
  std::future<void> close();
  void poll(size_t receive_frames_per_track = 4);

  MediaOperation publish_media(MediaPublishSpec);
  MediaOperation publish_video(MediaPublishSpec);
  MediaOperation publish_audio(MediaPublishSpec);
  MediaOperation subscribe_media(MediaSubscribeSpec);
  std::shared_ptr<SharedMediaInput> create_shared_input(TrackDescriptor, MediaInput);

  // Explicit advanced encoded/graph API. All tracks share the same room registry.
  std::future<TrackId> publish(TrackDescriptor,
                               std::shared_ptr<graphflow::core::GraphNode> feedback_sink = {});
  std::future<TrackId> subscribe(TrackDescriptor,
                                 std::shared_ptr<graphflow::core::GraphNode> consumer);
  std::future<TrackId> publish_graph(TrackDescriptor, PublishGraphFactory);
  std::future<TrackId> subscribe_graph(TrackDescriptor, SubscribeGraphFactory);
  std::future<void> unpublish(const TrackId&);
  std::future<void> unsubscribe(const TrackId&);
  std::future<void> set_muted(const TrackId&, bool, ControlOptions = {});
  std::future<void> set_receive_demand(const TrackId&, ReceiveDemand, ControlOptions = {});
  // Producer threads may write; closed/absent publications return WriteStatus::closed.
  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  SenderFeedback feedback(const TrackId&) const;

  struct GraphFailure {
    TrackId id;
    bool publication;
    std::string message;
  };

  std::optional<GraphFailure> next_failure();

  // Borrowed view valid until engine destruction. No room/track teardown escape hatch.
  const RoomView& room() const;
  // Single-consumer room event queue. Internal modules observe endpoints, not this queue.
  std::optional<RoomEvent> next_event(std::chrono::milliseconds timeout = {},
                                      std::stop_token stop = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Encoded-only factory links LiveKit::RTCEngine; overload with audio options
// installs media support and links LiveKit::RTCMediaEngine.
std::unique_ptr<RtcEngine> create_livekit_engine();
std::unique_ptr<RtcEngine> create_livekit_engine(AudioOutputOptions);
}  // namespace livekit::sdk::rtc::engine
