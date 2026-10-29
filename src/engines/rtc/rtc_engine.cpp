#include "livekit/rtc/engine.hpp"
#include "engines/rtc/rtc_engine_impl.hpp"
#include "backends/rtc/room_transport.hpp"
#include "livekit/error.hpp"

namespace livekit::sdk::rtc::engine {
RtcEngine::RtcEngine(RoomEngine::TransportFactory factory)
    : RtcEngine(std::make_unique<RoomEngine>(std::move(factory))) {}

RtcEngine::RtcEngine(std::unique_ptr<RoomEngine> room)
    : impl_(std::make_unique<Impl>(std::move(room))) {}

RtcEngine::~RtcEngine() = default;

RtcEngine::Impl::Impl(std::unique_ptr<RoomEngine> value)
    : room(std::move(value)), room_view(room.get()) {
  if (!room) {
    throw std::invalid_argument("RTC engine requires a room engine");
  }
  pipelines = std::make_unique<RtcPipelineEngine>(*room);
}

RtcEngine::Impl::~Impl() {
  // Clear graphs while media callbacks and the room still exist. Room destruction
  // supplies the final transport barrier even if a shutdown operation fails.
  try {
    clear_tracks();
    room->leave().get();
  } catch (...) {
  }
}

void RtcEngine::Impl::require_owner() const {
  if (std::this_thread::get_id() != owner) {
    throw std::logic_error("RTC control requires its owner thread");
  }
  if (closed) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed, "RTC engine closed");
  }
}

MediaModule& RtcEngine::Impl::require_media() const {
  require_owner();
  if (!media) {
    throw SDKError(ErrorDomain::media, ErrorCode::unsupported, "RTC media module is not enabled");
  }
  return *media;
}

void RtcEngine::Impl::clear_tracks() {
  pipelines->clear();
  if (media) {
    media->finish_pending();
  }
}

bool RtcEngine::media_enabled() const {
  impl_->require_owner();
  return bool(impl_->media);
}

std::future<void> RtcEngine::join(RoomOptions options) {
  impl_->require_owner();
  const auto state = impl_->room->snapshot().state;
  if (state == ConnectionState::failed || state == ConnectionState::closed) {
    impl_->clear_tracks();
  }
  return impl_->room->join(std::move(options));
}

std::future<void> RtcEngine::leave() {
  impl_->require_owner();
  impl_->clear_tracks();
  return impl_->room->leave();
}

std::future<void> RtcEngine::close() {
  if (std::this_thread::get_id() != impl_->owner) {
    throw std::logic_error("RTC close requires its owner thread");
  }
  impl_->closed = true;
  impl_->clear_tracks();
  return impl_->room->leave();
}

void RtcEngine::poll(size_t frames) {
  impl_->require_owner();
  impl_->pipelines->poll(frames);
  if (impl_->media) {
    impl_->media->finish_pending();
  }
}

MediaOperation RtcEngine::publish_media(MediaPublishSpec spec) {
  return impl_->require_media().publish_media(std::move(spec));
}

MediaOperation RtcEngine::publish_video(MediaPublishSpec spec) {
  if (spec.track.kind != MediaKind::video) {
    throw std::invalid_argument("Expected video publication");
  }
  return publish_media(std::move(spec));
}

MediaOperation RtcEngine::publish_audio(MediaPublishSpec spec) {
  if (spec.track.kind != MediaKind::audio) {
    throw std::invalid_argument("Expected audio publication");
  }
  return publish_media(std::move(spec));
}

MediaOperation RtcEngine::subscribe_media(MediaSubscribeSpec spec) {
  return impl_->require_media().subscribe_media(std::move(spec));
}

std::shared_ptr<SharedMediaInput> RtcEngine::create_shared_input(TrackDescriptor format,
                                                                 MediaInput input) {
  return impl_->require_media().create_shared_input(std::move(format), std::move(input));
}

std::future<TrackId> RtcEngine::publish(TrackDescriptor descriptor,
                                        std::shared_ptr<graphflow::core::GraphNode> sink) {
  impl_->require_owner();
  return impl_->pipelines->publish(std::move(descriptor), std::move(sink));
}

std::future<TrackId> RtcEngine::subscribe(TrackDescriptor descriptor,
                                          std::shared_ptr<graphflow::core::GraphNode> consumer) {
  impl_->require_owner();
  return impl_->pipelines->subscribe(std::move(descriptor), std::move(consumer));
}

std::future<TrackId> RtcEngine::publish_graph(TrackDescriptor descriptor,
                                              PublishGraphFactory factory) {
  impl_->require_owner();
  return impl_->pipelines->publish_graph(std::move(descriptor), std::move(factory));
}

std::future<TrackId> RtcEngine::subscribe_graph(TrackDescriptor descriptor,
                                                SubscribeGraphFactory factory) {
  impl_->require_owner();
  return impl_->pipelines->subscribe_graph(std::move(descriptor), std::move(factory));
}

std::future<void> RtcEngine::unpublish(const TrackId& id) {
  impl_->require_owner();
  auto result = impl_->pipelines->unpublish(id);
  if (impl_->media) {
    impl_->media->finish_pending();
  }
  return result;
}

std::future<void> RtcEngine::unsubscribe(const TrackId& id) {
  impl_->require_owner();
  auto result = impl_->pipelines->unsubscribe(id);
  if (impl_->media) {
    impl_->media->finish_pending();
  }
  return result;
}

std::future<void> RtcEngine::set_muted(const TrackId& id, bool muted, ControlOptions options) {
  impl_->require_owner();
  return impl_->room->set_muted(id, muted, options);
}

std::future<void> RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand,
                                                ControlOptions options) {
  impl_->require_owner();
  return impl_->room->set_receive_demand(id, demand, options);
}

WriteStatus RtcEngine::write(const TrackId& id, EncodedFrame frame) {
  return impl_->pipelines->write(id, std::move(frame));
}

TrackSnapshot RtcEngine::publication(const TrackId& id) const {
  return impl_->pipelines->publication(id);
}

SenderFeedback RtcEngine::feedback(const TrackId& id) const {
  return impl_->pipelines->feedback(id);
}

std::optional<RtcEngine::GraphFailure> RtcEngine::next_failure() {
  impl_->require_owner();
  if (auto failure = impl_->pipelines->next_failure()) {
    return GraphFailure{failure->id, failure->publication, std::move(failure->message)};
  }
  return {};
}

const RoomView& RtcEngine::room() const {
  return impl_->room_view;
}

std::optional<RoomEvent> RtcEngine::next_event(std::chrono::milliseconds timeout,
                                               std::stop_token stop) {
  return impl_->room->next_event(timeout, stop);
}
}  // namespace livekit::sdk::rtc::engine
