#pragma once
#include "livekit/rtc/engine.hpp"
#include "engines/rtc/pipeline_engine.hpp"
#include "engines/rtc/media_module.hpp"
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
  MediaModule& require_media() const;
  Result<void> clear_tracks();

  template <class Function>
  auto execute(Function function) const {
    return detail::control_boundary([&] {
      detail::OperationGuard guard(room->control());
      pipelines->reap_failed();
      return function();
    });
  }

  std::shared_ptr<std::atomic<bool>> callback_active;
  bool closed = false;
  std::atomic<bool> has_media{false};
  std::unique_ptr<RoomEngine> room;
  std::unique_ptr<RtcPipelineEngine> pipelines;
  std::unique_ptr<MediaModule> media;
  RoomView room_view;
};
}  // namespace livekit::sdk::rtc::engine
