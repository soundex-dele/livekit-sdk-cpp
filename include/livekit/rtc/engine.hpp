#pragma once
#include "livekit/rtc/room_engine.hpp"
#include "livekit/rtc/media_config.hpp"
#include "livekit/rtc/engine_observer.hpp"
#include <memory>
#include <optional>
#include <string>

namespace livekit::sdk::rtc::engine {
class RtcGraphApi;

// Single-room facade. Network/track controls enqueue work and return admission
// status, never the operation result. Register observers before submitting.
// Same-object/same-kind duplicates return busy. Completion is delivered once per
// accepted request to its original observer (if alive). See observer.hpp for
// threading/lifetime rules. leave/close interrupt and cancel pending work.
// Stop application callers before destruction; never destroy from a callback.
class RtcEngine final {
 public:
  explicit RtcEngine(RoomEngine::TransportFactory);
  explicit RtcEngine(std::unique_ptr<RoomEngine>);
  ~RtcEngine();
  RtcEngine(const RtcEngine&) = delete;
  RtcEngine& operator=(const RtcEngine&) = delete;

  // Links LiveKit::RTCMediaEngine. Call before join; repeated installation is rejected.
  ErrorCode enable_media(AudioOutputOptions = {});
  bool media_enabled() const;
  // Thread-safe local policy observation; not a room-wide transition ACK.
  VideoCodecPolicySnapshot video_codec_policy() const;
  ErrorCode join(RoomOptions);
  // Cancels preceding requests, then quiesces graphs before leaving.
  ErrorCode leave();
  // Terminal. Further submissions return session_closed.
  ErrorCode close();

  ErrorCode publish_media(MediaPublishSpec);
  ErrorCode publish_video(MediaPublishSpec);
  ErrorCode publish_audio(MediaPublishSpec);
  ErrorCode subscribe_media(MediaSubscribeSpec);
  ErrorCode create_shared_input(TrackDescriptor, MediaInput);

  // Encoded publication. Advanced graph composition requires graph_api.hpp.
  ErrorCode publish(TrackDescriptor);
  RtcGraphApi graph();
  ErrorCode unpublish(const TrackId&);
  ErrorCode unsubscribe(const TrackId&);
  ErrorCode set_muted(const TrackId&, bool, ControlOptions = {});
  ErrorCode set_receive_demand(const TrackId&, ReceiveDemand, ControlOptions = {});
  // Producer threads may write; closed/absent publications return WriteStatus::closed.
  WriteStatus write(const TrackId&, EncodedFrame);
  TrackSnapshot publication(const TrackId&) const;
  SenderFeedback feedback(const TrackId&) const;

  using GraphFailure = engine::GraphFailure;
  void set_room_observer(std::weak_ptr<RoomObserver>);
  void set_publication_observer(std::weak_ptr<PublicationObserver>);
  void set_subscription_observer(std::weak_ptr<SubscriptionObserver>);

  // Borrowed view valid until engine destruction. No room/track teardown escape hatch.
  const RoomView& room() const;

 private:
  friend class RtcGraphApi;
  Result<MediaPublication> start_media(MediaPublishSpec, std::optional<MediaKind> expected = {});
  Result<std::shared_ptr<SharedMediaInput>> create_shared_input_impl(TrackDescriptor, MediaInput);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Encoded-only factory links LiveKit::RTCEngine; overload with audio options
// installs media support and links LiveKit::RTCMediaEngine.
Result<std::unique_ptr<RtcEngine>> create_livekit_engine();
Result<std::unique_ptr<RtcEngine>> create_livekit_engine(AudioOutputOptions);
}  // namespace livekit::sdk::rtc::engine
