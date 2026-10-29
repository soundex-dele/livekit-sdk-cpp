#pragma once
#include "livekit/rtc/engine.hpp"
#include <functional>
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
struct ExecutorResult<std::future<T>> {
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

// Optional bounded command/future adapter using utoolkit task_queue. RtcEngine
// itself progresses autonomously; this adapter is not a media driver. submit() may be
// called from any thread; functions execute serially and must not block or retain
// a borrowed engine reference. Returning a future is supported and flattened:
// never call get() in the submitted function. Use shutdown() for terminal close.
class RtcExecutor final {
 public:
  using Factory = std::function<std::unique_ptr<RtcEngine>()>;
  explicit RtcExecutor(Factory, ExecutorOptions = {});
  ~RtcExecutor();
  RtcExecutor(const RtcExecutor&) = delete;
  RtcExecutor& operator=(const RtcExecutor&) = delete;

  template <class Function>
  auto submit(Function&& function) -> std::future<
      typename executor_detail::ExecutorResult<std::invoke_result_t<Function, RtcEngine&>>::Type> {
    using Return = std::invoke_result_t<Function, RtcEngine&>;
    using Value = typename executor_detail::ExecutorResult<Return>::Type;
    static_assert(!std::is_reference_v<Value>, "Executor results must own their lifetime");
    auto promise = std::make_shared<std::promise<Value>>();
    auto future = promise->get_future();
    auto call = std::make_shared<std::decay_t<Function>>(std::forward<Function>(function));
    enqueue({[call, promise](RtcEngine& engine) -> std::function<bool()> {
               if constexpr (executor_detail::ExecutorResult<Return>::future) {
                 auto pending = std::make_shared<Return>((*call)(engine));
                 return [pending, promise] {
                   if (pending->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                     return false;
                   }
                   if constexpr (std::is_void_v<Value>) {
                     pending->get();
                     promise->set_value();
                   } else {
                     promise->set_value(pending->get());
                   }
                   return true;
                 };
               } else {
                 if constexpr (std::is_void_v<Value>) {
                   (*call)(engine);
                   promise->set_value();
                 } else {
                   promise->set_value((*call)(engine));
                 }
                 return {};
               }
             },
             [promise](std::exception_ptr error) { promise->set_exception(error); }});
    return future;
  }

  ExecutorSnapshot snapshot() const;
  // Cancels queued/waiting operations, quiesces graphs and joins the owner.
  // Call from the owning application, never from a media callback or submit().
  void shutdown();

 private:
  struct Command {
    std::function<std::function<bool()>(RtcEngine&)> start;
    std::function<void(std::exception_ptr)> cancel;
  };

  void enqueue(Command);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::unique_ptr<RtcExecutor> create_livekit_executor(ExecutorOptions = {});
std::unique_ptr<RtcExecutor> create_livekit_executor(AudioOutputOptions, ExecutorOptions = {});
}  // namespace livekit::sdk::rtc::engine
