#pragma once
#include "livekit/rtc/engine.hpp"
#include "engines/rtc/pipeline_engine.hpp"
#include "engines/rtc/media_module.hpp"
#include "support/task_dispatcher.hpp"
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
    if (control_is_current()) {
      (*task)();
    } else {
      dispatch([task] { (*task)(); });
    }
    return future.get();
  }

  template <class Function>
  auto submit(Function fn) const {
    CompletionSource<void> admitted;
    auto result = admitted.get_result().then(
        [this](std::function<void()> task) { dispatch(std::move(task)); },
        [fn = std::move(fn)](AsyncResult<void> ready) mutable {
          ready.get();
          return fn();
        });
    admitted.set_value();
    return result;
  }

  bool control_is_current() const;
  void dispatch(std::function<void()>) const;

  std::unique_ptr<vi::TaskQueue> control;
  mutable std::atomic<size_t> queued_commands{0};
  livekit::sdk::detail::TaskDispatcher completion_dispatch;
  std::shared_ptr<std::atomic<bool>> callback_active;
  bool stopping = false;
  bool closed = false;
  std::atomic<bool> has_media{false};
  std::unique_ptr<RoomEngine> room;
  std::unique_ptr<RtcPipelineEngine> pipelines;
  std::unique_ptr<MediaModule> media;
  RoomView room_view;
};
}  // namespace livekit::sdk::rtc::engine
