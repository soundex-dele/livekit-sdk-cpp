#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/media_config.hpp"
#include <memory>
#include <optional>
#include <string>

namespace livekit::sdk::rtc::engine {
class RtcGraphApi;

// Single-room facade. Completed Result values are returned on the calling thread.
// Controls share the room's non-recursive operation lock, including protocol waits.
// Call from application workers, never from notifications/media callbacks or hooks.
// Stop and join all callers before destruction; close does not preempt an operation.
class RtcEngine final {
 public:
  explicit RtcEngine(RoomEngine::TransportFactory);
  explicit RtcEngine(std::unique_ptr<RoomEngine>);
  ~RtcEngine();
  RtcEngine(const RtcEngine&) = delete;
  RtcEngine& operator=(const RtcEngine&) = delete;

  // Links LiveKit::RTCMediaEngine. Call before join; repeated installation is rejected.
  Result<void> enable_media(AudioOutputOptions = {});
  bool media_enabled() const;
  Result<void> join(RoomOptions);
  // Waits for preceding operations, then quiesces graphs before leaving.
  Result<void> leave();
  // Terminal and idempotent. Rejects subsequent control operations.
  Result<void> close();

  Result<MediaPublication> publish_media(MediaPublishSpec);
  Result<MediaPublication> publish_video(MediaPublishSpec);
  Result<MediaPublication> publish_audio(MediaPublishSpec);
  Result<MediaSubscription> subscribe_media(MediaSubscribeSpec);
  Result<std::shared_ptr<SharedMediaInput>> create_shared_input(TrackDescriptor, MediaInput);

  // Encoded publication. Advanced graph composition requires graph_api.hpp.
  Result<TrackId> publish(TrackDescriptor);
  RtcGraphApi graph();
  Result<void> unpublish(const TrackId&);
  Result<void> unsubscribe(const TrackId&);
  Result<void> set_muted(const TrackId&, bool, ControlOptions = {});
  Result<void> set_receive_demand(const TrackId&, ReceiveDemand, ControlOptions = {});
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

  // Serial notification task queue, separate from application callers and media workers.
  // Register before join. Replacing/clearing callbacks fences old invocations.
  // Synchronous control and destruction from a callback are forbidden.
  // Serialize registration calls. Callback arguments are borrowed for the call.
  // Callbacks must return promptly; throwing disables them and fails the room.
  Result<void> set_callbacks(Callbacks);

  // Borrowed view valid until engine destruction. No room/track teardown escape hatch.
  const RoomView& room() const;

 private:
  friend class RtcGraphApi;
  Result<MediaPublication> start_media(MediaPublishSpec, std::optional<MediaKind> expected = {});
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Encoded-only factory links LiveKit::RTCEngine; overload with audio options
// installs media support and links LiveKit::RTCMediaEngine.
Result<std::unique_ptr<RtcEngine>> create_livekit_engine();
Result<std::unique_ptr<RtcEngine>> create_livekit_engine(AudioOutputOptions);
}  // namespace livekit::sdk::rtc::engine
