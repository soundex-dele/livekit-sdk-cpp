#pragma once
// Non-installed contract-test/supervisor host. Production uses RtcEngine only.
#include "engines/rtc/rtc_execution_context.hpp"
#include "backends/rtc/livekit/transport_factory.hpp"

namespace livekit::sdk::rtc {
class RoomFixture {
 public:
  using TransportFactory = RoomEngine::TransportFactory;

  explicit RoomFixture(TransportFactory factory) : context_(std::move(factory)) {
    context_.set_event_handler([this](const RoomEvent& event) {
      if (!event.publication) {
        if (auto observer = room_observer_.get().lock()) {
          observer->on_room_event(event);
        }
      } else if (*event.publication) {
        if (auto observer = publication_observer_.get().lock()) {
          observer->on_publication_event(event);
        }
      } else if (auto observer = subscription_observer_.get().lock()) {
        observer->on_subscription_event(event);
      }
    });
  }

  ~RoomFixture() {
    context_.control()->async->shutdown();
    context_.set_event_handler({});
  }

  RoomSnapshot snapshot() const {
    return context_.snapshot();
  }

  Result<std::shared_ptr<RoomServices>> services() const {
    return context_.services();
  }

  void set_room_observer(std::weak_ptr<RoomObserver> value) {
    room_observer_.set(std::move(value));
  }

  void set_publication_observer(std::weak_ptr<PublicationObserver> value) {
    publication_observer_.set(std::move(value));
  }

  void set_subscription_observer(std::weak_ptr<SubscriptionObserver> value) {
    subscription_observer_.set(std::move(value));
  }

  ErrorCode join(RoomOptions options, Completion<void> completion) {
    if (options.video_codec_policy) {
      return ErrorCode::unsupported;
    }
    auto key = "room.join";
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<void>([&](RoomEngine& room, auto done) {
              room.join(std::move(options), std::move(done));
            });
          },
          std::move(completion));
    });
  }

  ErrorCode publish(TrackDescriptor descriptor,
                    Completion<std::shared_ptr<PublishedTrack>> completion) {
    auto key = "publication.publish:" + descriptor.id;
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<std::shared_ptr<PublishedTrack>>(
                [&](RoomEngine& room, auto done) {
                  room.publish(std::move(descriptor), std::move(done));
                });
          },
          std::move(completion));
    });
  }

  ErrorCode subscribe(TrackDescriptor descriptor,
                      Completion<std::shared_ptr<SubscribedTrack>> completion) {
    auto key = "subscription.subscribe:" + descriptor.id;
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<std::shared_ptr<SubscribedTrack>>(
                [&](RoomEngine& room, auto done) {
                  room.subscribe(std::move(descriptor), std::move(done));
                });
          },
          std::move(completion));
    });
  }

  ErrorCode unpublish(const TrackId& id, Completion<void> completion) {
    auto key = "publication.remove:" + id;
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<void>(
                [&](RoomEngine& room, auto done) { room.unpublish(id, std::move(done)); });
          },
          std::move(completion));
    });
  }

  ErrorCode unsubscribe(const TrackId& id, Completion<void> completion) {
    auto key = "subscription.remove:" + id;
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<void>(
                [&](RoomEngine& room, auto done) { room.unsubscribe(id, std::move(done)); });
          },
          std::move(completion));
    });
  }

  ErrorCode set_muted(const TrackId& id, bool muted, Completion<void> completion,
                      ControlOptions options = {}) {
    auto key = "publication.mute:" + id;
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<void>([&](RoomEngine& room, auto done) {
              room.set_muted(id, muted, options, std::move(done));
            });
          },
          std::move(completion));
    });
  }

  ErrorCode set_receive_demand(const TrackId& id, ReceiveDemand demand, Completion<void> completion,
                               ControlOptions options = {}) {
    auto key = "subscription.demand:" + id;
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<void>([&](RoomEngine& room, auto done) {
              room.set_receive_demand(id, demand, options, std::move(done));
            });
          },
          std::move(completion));
    });
  }

  ErrorCode update_local_participant(ParticipantUpdate update, Completion<void> completion,
                                     ControlOptions options = {}) {
    auto key = "room.participant";
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<void>([&](RoomEngine& room, auto done) {
              room.update_local_participant(std::move(update), options, std::move(done));
            });
          },
          std::move(completion));
    });
  }

  ErrorCode set_subscription_permissions(SubscriptionPermissions policy,
                                         Completion<void> completion, ControlOptions options = {}) {
    auto key = "room.permissions";
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          std::move(key),
          [=, this]() mutable {
            return context_.invoke<void>([&](RoomEngine& room, auto done) {
              room.set_subscription_permissions(std::move(policy), options, std::move(done));
            });
          },
          std::move(completion));
    });
  }

  ErrorCode leave(Completion<void> completion = {}) {
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          "room.leave",
          [this] {
            return context_.lifecycle().submit([this] {
              return context_.invoke<void>(
                  [](RoomEngine& room, auto done) { room.leave(std::move(done)); });
            });
          },
          std::move(completion), true, false);
    });
  }

  ErrorCode close(Completion<void> completion = {}) {
    return detail::control_boundary([&] {
      return context_.control()->async->submit(
          "room.close",
          [this] {
            return context_.lifecycle().submit([this] {
              return context_.invoke<void>(
                  [](RoomEngine& room, auto done) { room.leave(std::move(done)); });
            });
          },
          std::move(completion), true, true);
    });
  }

 private:
  detail::ObserverSlot<RoomObserver> room_observer_;
  detail::ObserverSlot<PublicationObserver> publication_observer_;
  detail::ObserverSlot<SubscriptionObserver> subscription_observer_;
  detail::RtcExecutionContext context_;
};
}  // namespace livekit::sdk::rtc
