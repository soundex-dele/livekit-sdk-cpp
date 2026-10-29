#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/media_config.hpp"
#include <memory>
#include <optional>
#include <string>

namespace livekit::sdk::rtc::engine {
class RtcGraphApi;

// Single-room facade with an owned serial task queue. Control methods marshal
// to that queue without waiting for command execution. Results progress
// automatically. Media/feedback source nodes dispatch on their own workers.
// Never wait for lifecycle operations from media callbacks or graph hooks.
// Destruction requires callers to have stopped using this facade.
class RtcEngine final {
 public:
  explicit RtcEngine(RoomEngine::TransportFactory);
  explicit RtcEngine(std::unique_ptr<RoomEngine>);
  ~RtcEngine();
  RtcEngine(const RtcEngine&) = delete;
  RtcEngine& operator=(const RtcEngine&) = delete;

  // Links LiveKit::RTCMediaEngine. Call before join; repeated installation is rejected.
  AsyncResult<void> enable_media(AudioOutputOptions = {});
  bool media_enabled() const;
  AsyncResult<void> join(RoomOptions);
  // Quiesces graphs before leaving. Await completion before joining again.
  AsyncResult<void> leave();
  // Terminal and idempotent. Rejects subsequent control operations.
  AsyncResult<void> close();

  MediaOperation publish_media(MediaPublishSpec);
  MediaOperation publish_video(MediaPublishSpec);
  MediaOperation publish_audio(MediaPublishSpec);
  MediaOperation subscribe_media(MediaSubscribeSpec);
  AsyncResult<std::shared_ptr<SharedMediaInput>> create_shared_input(TrackDescriptor, MediaInput);

  // Encoded publication. Advanced graph composition requires graph_api.hpp.
  AsyncResult<TrackId> publish(TrackDescriptor);
  RtcGraphApi graph();
  AsyncResult<void> unpublish(const TrackId&);
  AsyncResult<void> unsubscribe(const TrackId&);
  AsyncResult<void> set_muted(const TrackId&, bool, ControlOptions = {});
  AsyncResult<void> set_receive_demand(const TrackId&, ReceiveDemand, ControlOptions = {});
  // Producer threads may write; closed/absent publications return WriteStatus::closed.
  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  SenderFeedback feedback(const TrackId&) const;

  struct GraphFailure {
    TrackId id;
    bool publication;
    std::string message;
    // The same failure may also arrive as a RoomEvent. Deduplicate by failure->id;
    // no ordering between the two notification paths is guaranteed.
    std::shared_ptr<const FailureInfo> failure;
  };

  struct Callbacks {
    std::function<void(const RoomEvent&)> on_event;
    std::function<void(const GraphFailure&)> on_failure;
  };

  // Serial notification task queue, separate from control and media workers.
  // Register before join. Replacing/clearing callbacks fences old invocations.
  // Control calls are permitted; destruction from a callback is not permitted.
  // Serialize registration calls. Callback arguments are borrowed for the call.
  // Callbacks must return promptly; throwing disables them and fails the room.
  void set_callbacks(Callbacks);

  // Borrowed view valid until engine destruction. No room/track teardown escape hatch.
  const RoomView& room() const;

 private:
  friend class RtcGraphApi;
  friend class RtcExecutor;
  MediaOperation start_media(MediaPublishSpec, std::optional<MediaKind> expected = {});
  void post_control(std::function<void()>);
  bool on_control_thread() const;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Encoded-only factory links LiveKit::RTCEngine; overload with audio options
// installs media support and links LiveKit::RTCMediaEngine.
std::unique_ptr<RtcEngine> create_livekit_engine();
std::unique_ptr<RtcEngine> create_livekit_engine(AudioOutputOptions);
}  // namespace livekit::sdk::rtc::engine
