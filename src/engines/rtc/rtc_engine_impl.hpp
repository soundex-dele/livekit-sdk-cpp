#pragma once
#include "livekit/rtc/engine.hpp"
#include "engines/rtc/track_graph_registry.hpp"
#include "engines/rtc/media_graph_contract.hpp"
#include "backends/rtc/control_context.hpp"
#include <memory>
#include <thread>
#include <atomic>

namespace livekit::sdk::rtc::engine {
// Shared only by the core and optional media implementation translation units.
// Owns all state; destruction quiesces graphs before releasing media and room.
struct RtcEngine::Impl {
  explicit Impl(std::unique_ptr<RoomEngine>);
  ~Impl();
  void require_open() const;
  MediaGraphBuilder& require_media() const;
  Result<void> clear_tracks();
  Result<void> detach(TrackGraphRegistry::RetiredGraphs) const;
  Result<TrackId> publish_graph(TrackDescriptor, PublishGraphFactory);
  Result<TrackId> subscribe_graph(TrackDescriptor, SubscribeGraphFactory);
  Result<TrackId> publish(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> = {});
  Result<TrackId> subscribe(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode>);

  template <class Function>
  auto execute(Function function) const {
    return detail::control_boundary([&] {
      detail::OperationGuard guard(*control);
      auto retired = graphs->take_failed();
      auto detached = detach(std::move(retired));
      if (!detached) {
        throw SDKError(detached.failure_record());
      }
      return function();
    });
  }

  std::shared_ptr<std::atomic<bool>> callback_active;
  bool closed = false;
  std::atomic<bool> has_media{false};
  std::shared_ptr<detail::SessionControl> control;
  std::unique_ptr<RoomEngine> room;
  std::unique_ptr<TrackGraphRegistry> graphs;
  std::unique_ptr<MediaResources> media_resources;
  std::unique_ptr<MediaGraphBuilder> media;
  RoomView room_view;
};
}  // namespace livekit::sdk::rtc::engine
