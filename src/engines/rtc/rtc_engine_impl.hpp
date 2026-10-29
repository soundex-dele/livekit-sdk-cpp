#pragma once
#include "livekit/rtc/engine.hpp"
#include "engines/rtc/pipeline_engine.hpp"
#include "engines/rtc/media_module.hpp"
#include <memory>
#include <thread>

namespace livekit::sdk::rtc::engine {
// Shared only by the core and optional media implementation translation units.
// Owns all state; destruction quiesces graphs before releasing media and room.
struct RtcEngine::Impl {
  explicit Impl(std::unique_ptr<RoomEngine>);
  ~Impl();
  void require_owner() const;
  MediaModule& require_media() const;
  void clear_tracks();

  const std::thread::id owner = std::this_thread::get_id();
  bool closed = false;
  std::unique_ptr<RoomEngine> room;
  std::unique_ptr<RtcPipelineEngine> pipelines;
  std::unique_ptr<MediaModule> media;
  RoomView room_view;
};
}  // namespace livekit::sdk::rtc::engine
