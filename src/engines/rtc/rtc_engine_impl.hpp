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
  ErrorCode clear_tracks();
  ErrorCode detach(TrackGraphRegistry::RetiredGraphs) const;
  Result<TrackId> publish_graph(TrackDescriptor, PublishGraphFactory);
  Result<TrackId> subscribe_graph(TrackDescriptor, SubscribeGraphFactory);
  Result<TrackId> publish(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode> = {});
  Result<TrackId> subscribe(TrackDescriptor, std::shared_ptr<graphflow::core::GraphNode>);

  template <class Function>
  auto execute(Function function, bool allow_closed = false) const {
    using Value = std::invoke_result_t<Function>;
    using Return = std::conditional_t<std::is_void_v<Value>, ErrorCode, Value>;
    const auto fail = [](ErrorCode code, std::string message) -> Return {
      if constexpr (std::is_same_v<Return, ErrorCode>) {
        return code;
      } else {
        return Return::failure(std::make_shared<FailureInfo>(
            FailureInfo{next_failure_id(), ErrorDomain::lifecycle, code, std::move(message)}));
      }
    };
    return detail::control_boundary([&]() -> Return {
      if (detail::callback_depth || detail::active_session) {
        return fail(ErrorCode::invalid_thread,
                    "RTC control cannot be called from callbacks or nested sessions");
      }
      detail::OperationGuard guard(*control);
      if (control->closed && !allow_closed) {
        return fail(ErrorCode::session_closed, "RTC engine closed");
      }
      auto detached = detach(graphs->take_failed());
      if (detached != ErrorCode::ok) {
        return fail(detached, "RTC track detachment failed");
      }
      if constexpr (std::is_void_v<Value>) {
        function();
        return ErrorCode::ok;
      } else {
        return function();
      }
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
