#include "livekit/rtc/engine.hpp"
#include "livekit/rtc/graph_api.hpp"
#include "engines/rtc/media_graph_contract.hpp"

namespace livekit::sdk::rtc::engine {
RtcEngine::RtcEngine(TransportFactory factory)
    : room_(std::make_unique<RoomEngine>(std::move(factory))) {
  room_->set_observer(this);
}

RtcEngine::~RtcEngine() {
  room_->set_observer(nullptr);
  clear_tracks();
  room_->leave();
}

void RtcEngine::set_callbacks(RtcCallbacks callbacks) {
  callbacks_ = std::move(callbacks);
}

void RtcEngine::join(RoomOptions options) {
  room_->join(std::move(options));
}

void RtcEngine::clear_tracks() {
  for (auto& [id, entry] : publications_) {
    if (entry.pipeline) {
      (void)entry.pipeline->close_locked();
    }
  }
  for (auto& [id, entry] : subscriptions_) {
    if (entry.pipeline) {
      (void)entry.pipeline->close_locked();
    }
  }
  publications_.clear();
  subscriptions_.clear();
}

void RtcEngine::leave() {
  clear_tracks();
  room_->leave();
}

void RtcEngine::close() {
  leave();
}

RoomSnapshot RtcEngine::snapshot() const {
  return room_->snapshot();
}

std::shared_ptr<RoomServices> RtcEngine::services() const {
  return room_->services();
}

bool RtcEngine::media_enabled() const {
  return bool(media_);
}

void RtcEngine::publish(TrackDescriptor descriptor) {
  publish_graph(std::move(descriptor), [](auto) { return TrackGraphSpec{}; });
}

void RtcEngine::subscribe(TrackDescriptor descriptor) {
  subscribe_graph(std::move(descriptor), [](auto) { return TrackGraphSpec{}; });
}

void RtcEngine::subscribe(const TrackId& id) {
  subscriptions_[id].factory = [](auto) { return TrackGraphSpec{}; };
  room_->subscribe(id);
}

void RtcEngine::publish_graph(TrackDescriptor descriptor, PublishGraphFactory factory) {
  publications_[descriptor.id].factory = std::move(factory);
  room_->publish(std::move(descriptor));
}

void RtcEngine::subscribe_graph(TrackDescriptor descriptor, SubscribeGraphFactory factory) {
  subscriptions_[descriptor.id].factory = std::move(factory);
  room_->subscribe(std::move(descriptor));
}

void RtcEngine::publish_media(MediaPublishSpec spec) {
  auto descriptor = spec.track;
  publications_[descriptor.id].media = std::move(spec);
  room_->publish(std::move(descriptor));
}

void RtcEngine::publish_video(MediaPublishSpec spec) {
  spec.track.kind = MediaKind::video;
  publish_media(std::move(spec));
}

void RtcEngine::publish_audio(MediaPublishSpec spec) {
  spec.track.kind = MediaKind::audio;
  publish_media(std::move(spec));
}

void RtcEngine::subscribe_media(MediaSubscribeSpec spec) {
  auto descriptor = spec.track;
  subscriptions_[descriptor.id].media = std::move(spec);
  room_->subscribe(std::move(descriptor));
}

std::shared_ptr<SharedMediaInput> RtcEngine::create_shared_input(TrackDescriptor descriptor,
                                                                 MediaInput input) {
  return media_->create_shared_input(std::move(descriptor), std::move(input));
}

void RtcEngine::unpublish(const TrackId& id) {
  auto& entry = publications_.at(id);
  if (entry.pipeline) {
    (void)entry.pipeline->close_locked();
  }
  room_->unpublish(id);
}

void RtcEngine::unsubscribe(const TrackId& id) {
  auto& entry = subscriptions_.at(id);
  if (entry.pipeline) {
    (void)entry.pipeline->close_locked();
  }
  room_->unsubscribe(id);
}

void RtcEngine::set_muted(const TrackId& id, bool muted) {
  room_->set_muted(id, muted);
}

void RtcEngine::set_receive_demand(const TrackId& id, ReceiveDemand demand) {
  room_->set_receive_demand(id, demand);
}

void RtcEngine::update_local_participant(ParticipantUpdate update) {
  room_->update_local_participant(std::move(update));
}

void RtcEngine::set_subscription_permissions(SubscriptionPermissions permissions) {
  room_->set_subscription_permissions(std::move(permissions));
}

WriteStatus RtcEngine::write(const TrackId& id, EncodedFrame frame) {
  return publications_.at(id).pipeline->write(std::move(frame));
}

TrackSnapshot RtcEngine::publication(const TrackId& id) const {
  return publications_.at(id).pipeline->node().track()->snapshot();
}

SenderFeedback RtcEngine::feedback(const TrackId& id) const {
  return publications_.at(id).pipeline->node().track()->feedback();
}

void RtcEngine::on_joined(const RoomSnapshot& room) {
  if (callbacks_.on_joined) {
    callbacks_.on_joined(room);
  }
}

void RtcEngine::on_left() {
  if (callbacks_.on_left) {
    callbacks_.on_left();
  }
}

void RtcEngine::on_published(std::shared_ptr<PublishedTrack> endpoint) {
  const auto id = endpoint->descriptor().id;
  auto& entry = publications_.at(id);
  auto node = std::make_shared<nodes::RtcPublishNode>("rtc.publish." + id, endpoint);
  TrackGraphSpec spec;
  if (entry.media) {
    auto prepared = media_->build_publish(std::move(*entry.media), node);
    spec = std::move(prepared.graph);
    entry.track = std::move(prepared.track);
    entry.media.reset();
  } else {
    spec = entry.factory(node);
    entry.factory = {};
  }
  entry.pipeline = std::shared_ptr<PublishPipeline>(new PublishPipeline(node, std::move(spec), {}));
  entry.pipeline->start_locked();
  if (callbacks_.on_published) {
    callbacks_.on_published({id, entry.track});
  }
}

void RtcEngine::on_subscribed(std::shared_ptr<SubscribedTrack> endpoint) {
  const auto id = endpoint->descriptor().id;
  auto& entry = subscriptions_.at(id);
  auto node = std::make_shared<nodes::RtcSubscribeNode>("rtc.subscribe." + id, endpoint);
  TrackGraphSpec spec;
  if (entry.media) {
    auto prepared = media_->build_subscribe(std::move(*entry.media), node);
    spec = std::move(prepared.graph);
    entry.track = std::move(prepared.track);
    entry.media.reset();
  } else {
    spec = entry.factory(node);
    entry.factory = {};
  }
  entry.pipeline =
      std::shared_ptr<SubscribePipeline>(new SubscribePipeline(node, std::move(spec), {}));
  entry.pipeline->start_locked();
  if (callbacks_.on_subscribed) {
    callbacks_.on_subscribed({id, entry.track});
  }
}

void RtcEngine::on_unpublished(const TrackId& id) {
  publications_.erase(id);
  if (callbacks_.on_unpublished) {
    callbacks_.on_unpublished(id);
  }
}

void RtcEngine::on_unsubscribed(const TrackId& id) {
  subscriptions_.erase(id);
  if (callbacks_.on_unsubscribed) {
    callbacks_.on_unsubscribed(id);
  }
}

void RtcEngine::on_room_event(const RoomEvent& event) {
  if (event.kind == RoomEventKind::remote_track_added && callbacks_.on_track_published) {
    callbacks_.on_track_published(*event.descriptor);
  }
  if (callbacks_.on_event) {
    callbacks_.on_event(event);
  }
}

RtcGraphApi RtcEngine::graph() {
  return RtcGraphApi(*this);
}

void RtcGraphApi::publish_graph(TrackDescriptor descriptor, PublishGraphFactory factory) {
  engine_->publish_graph(std::move(descriptor), std::move(factory));
}

void RtcGraphApi::subscribe_graph(TrackDescriptor descriptor, SubscribeGraphFactory factory) {
  engine_->subscribe_graph(std::move(descriptor), std::move(factory));
}

void RtcGraphApi::publish(TrackDescriptor descriptor,
                          std::shared_ptr<graphflow::core::GraphNode> sink) {
  publish_graph(std::move(descriptor), [sink](auto node) {
    TrackGraphSpec spec;
    if (sink) {
      spec.nodes.push_back(sink);
      spec.connections.push_back(TrackGraphSpec::connect<nodes::SenderFeedbackEvent>(node, sink));
    }
    return spec;
  });
}

void RtcGraphApi::subscribe(TrackDescriptor descriptor,
                            std::shared_ptr<graphflow::core::GraphNode> consumer) {
  subscribe_graph(std::move(descriptor), [consumer](auto node) {
    TrackGraphSpec spec;
    spec.nodes.push_back(consumer);
    spec.connections.push_back(TrackGraphSpec::connect<nodes::EncodedFrameEvent>(node, consumer));
    return spec;
  });
}
}  // namespace livekit::sdk::rtc::engine
