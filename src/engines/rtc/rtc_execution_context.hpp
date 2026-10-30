#pragma once
#include "backends/rtc/room_engine.hpp"
#include "backends/rtc/async_control.hpp"
#include "backends/rtc/lifecycle_executor.hpp"

namespace livekit::sdk::rtc::detail {
struct RtcExecutionState;

// Execution resources owned by RtcEngine. No room business API: the facade
// supplies protocol work, while this object serializes access and hosts I/O and
// user notifications. RoomEngine itself has no knowledge of these executors.
class RtcExecutionContext {
 public:
  explicit RtcExecutionContext(RoomEngine::TransportFactory);
  ~RtcExecutionContext();
  RtcExecutionContext(const RtcExecutionContext&) = delete;
  RtcExecutionContext& operator=(const RtcExecutionContext&) = delete;

  std::shared_ptr<SessionControl> control() const;
  LifecycleExecutor& lifecycle();
  void set_event_handler(std::function<void(const RoomEvent&)>);
  void post_notification(std::function<void()>);
  RoomSnapshot snapshot() const;
  Result<std::shared_ptr<RoomServices>> services() const;

  // The callable runs synchronously under the owner's gate. Only its operation
  // result may outlive this call; callers must not retain references in callbacks.
  template <class T, class Function>
  auto invoke(Function function) {
    using Value = std::conditional_t<std::is_void_v<T>, ErrorCode, Result<T>>;
    auto source = std::make_shared<CompletionSource<Value>>();
    auto result = source->get_result();
    auto done = [source](Result<T> completed) {
      if constexpr (std::is_void_v<T>) {
        if (completed) {
          source->set_value(ErrorCode::ok);
        } else {
          source->set_exception(std::make_exception_ptr(SDKError(completed.failure_record())));
        }
      } else {
        source->set_value(std::move(completed));
      }
    };
    try {
      access([&](RoomEngine& room) { function(room, done); });
    } catch (...) {
      done(Result<T>::failure(describe_failure(std::current_exception())));
    }
    return result;
  }

 private:
  void access(const std::function<void(RoomEngine&)>&);
  std::shared_ptr<RtcExecutionState> state_;
};
}  // namespace livekit::sdk::rtc::detail
