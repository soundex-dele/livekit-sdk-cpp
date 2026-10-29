#pragma once
#include "livekit/rtc/engine.hpp"
#include "engines/rtc/pipeline_engine.hpp"
#include "engines/rtc/media_module.hpp"
#include <memory>
#include <thread>
#include <atomic>

namespace vi {
class TaskQueue;
}

namespace livekit::sdk::rtc::engine {
// Shared only by the core and optional media implementation translation units.
// Owns all state; destruction quiesces graphs before releasing media and room.
struct RtcEngine::Impl {
  explicit Impl(std::unique_ptr<RoomEngine>);
  ~Impl();
  void require_owner() const;
  MediaModule& require_media() const;
  void clear_tracks();

  // Only task-queue closures access lifecycle/module state. API calls marshal
  // here; transport/media workers never execute control callbacks directly.
  template <class Function>
  auto call(Function&& fn) const -> std::invoke_result_t<Function> {
    using Result = std::invoke_result_t<Function>;
    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(fn));
    auto future = task->get_future();
    dispatch([task] { (*task)(); });  // packaged_task captures exceptions.
    return future.get();
  }

  void dispatch(std::function<void()>) const;
  void completion_ready();
  void schedule_health();
  void fail_control(std::exception_ptr);

  std::unique_ptr<vi::TaskQueue> control;
  mutable std::atomic<size_t> queued_commands{0};
  std::atomic<bool> completion_queued{false};
  std::shared_ptr<std::atomic<bool>> callback_active;
  bool stopping = false;
  std::exception_ptr control_failure;
  bool closed = false;
  std::unique_ptr<RoomEngine> room;
  std::unique_ptr<RtcPipelineEngine> pipelines;
  std::unique_ptr<MediaModule> media;
  RoomView room_view;
};
}  // namespace livekit::sdk::rtc::engine
