#pragma once
#include "livekit/rtc/engine.hpp"
#include <functional>
#include <atomic>
#include <type_traits>
#include <utility>

namespace livekit::sdk::rtc::engine {
namespace executor_detail {
template <class T>
struct ExecutorResult {
  using Type = T;
  static constexpr bool future = false;
};

template <class T>
struct ExecutorResult<AsyncResult<T>> {
  using Type = T;
  static constexpr bool future = true;
};
}  // namespace executor_detail

struct ExecutorOptions {
  size_t max_operations = 1024;
};

struct ExecutorSnapshot {
  uint64_t commands_started = 0, commands_completed = 0;
  std::chrono::microseconds max_command_duration{0};
  size_t queued_operations = 0, pending_operations = 0;
  bool running = false;
  std::string error;
};

// Optional bounded command adapter reusing the engine task queue; no extra thread. RtcEngine
// itself progresses autonomously; this adapter is not a media driver. submit() may be
// called from any thread; functions execute serially and must not block or retain
// a borrowed engine reference. Returning AsyncResult is supported and flattened:
// never call get() in the submitted function. Use shutdown() for terminal close.
class RtcExecutor final {
 public:
  using Factory = std::function<std::unique_ptr<RtcEngine>()>;
  explicit RtcExecutor(Factory, ExecutorOptions = {});
  ~RtcExecutor();
  RtcExecutor(const RtcExecutor&) = delete;
  RtcExecutor& operator=(const RtcExecutor&) = delete;

  template <class Function>
  auto submit(Function&& function) -> AsyncResult<
      typename executor_detail::ExecutorResult<std::invoke_result_t<Function, RtcEngine&>>::Type> {
    using Return = std::invoke_result_t<Function, RtcEngine&>;
    using Value = typename executor_detail::ExecutorResult<Return>::Type;
    static_assert(!std::is_reference_v<Value>, "Executor results must own their lifetime");
    auto promise = std::make_shared<CompletionSource<Value>>();
    auto result = promise->get_result();
    auto settled = std::make_shared<std::atomic<bool>>(false);
    auto call = std::make_shared<std::decay_t<Function>>(std::forward<Function>(function));
    auto cancel = [promise, settled](std::exception_ptr failure) {
      if (!settled->exchange(true)) {
        promise->set_exception(failure);
      }
    };
    enqueue({[call, promise, settled, cancel](RtcEngine& engine, std::function<void()> finished) {
               try {
                 if constexpr (executor_detail::ExecutorResult<Return>::future) {
                   auto pending = (*call)(engine);
                   std::move(pending).then([](auto task) { task(); },
                                           [promise, settled, finished](AsyncResult<Value> ready) {
                                             if (!settled->exchange(true)) {
                                               try {
                                                 if constexpr (std::is_void_v<Value>) {
                                                   ready.get();
                                                   promise->set_value();
                                                 } else {
                                                   promise->set_value(ready.get());
                                                 }
                                               } catch (...) {
                                                 promise->set_exception(std::current_exception());
                                               }
                                             }
                                             finished();
                                           });
                 } else {
                   if constexpr (std::is_void_v<Value>) {
                     (*call)(engine);
                     if (!settled->exchange(true)) {
                       promise->set_value();
                     }
                   } else {
                     auto value = (*call)(engine);
                     if (!settled->exchange(true)) {
                       promise->set_value(std::move(value));
                     }
                   }
                   finished();
                 }
               } catch (...) {
                 cancel(std::current_exception());
                 finished();
               }
             },
             std::move(cancel)});
    return result;
  }

  ExecutorSnapshot snapshot() const;
  // Cancels queued/waiting operations, quiesces graphs and joins the owner.
  // Call from the owning application, never from a media callback or submit().
  void shutdown();

 private:
  struct Command {
    std::function<void(RtcEngine&, std::function<void()>)> start;
    std::function<void(std::exception_ptr)> cancel;
  };

  void enqueue(Command);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<RtcExecutor> create_livekit_executor(ExecutorOptions = {});
std::unique_ptr<RtcExecutor> create_livekit_executor(AudioOutputOptions, ExecutorOptions = {});
}  // namespace livekit::sdk::rtc::engine
