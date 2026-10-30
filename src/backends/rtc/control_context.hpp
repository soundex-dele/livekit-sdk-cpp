#pragma once

#include "livekit/result.hpp"
#include <mutex>
#include <atomic>

namespace livekit::sdk::rtc::detail {
// Scoped on every user callback and RTC worker. Never acquire an operation lock there.
inline thread_local unsigned callback_depth = 0;
inline thread_local const void* active_session = nullptr;
inline thread_local std::shared_ptr<const FailureInfo> control_failure;
class AsyncControl;

struct CallbackScope {
  CallbackScope() {
    ++callback_depth;
  }

  ~CallbackScope() {
    --callback_depth;
  }
};

struct SessionControl {
  std::mutex operation_mutex;
  bool closed = false;  // Protected by operation_mutex.
  std::atomic<bool> interrupted{false};
  std::shared_ptr<AsyncControl> async;
};

class OperationGuard {
 public:
  explicit OperationGuard(SessionControl& session)
      : lock_(session.operation_mutex, std::defer_lock) {
    if (callback_depth || active_session) {
      throw SDKError(ErrorDomain::lifecycle, ErrorCode::invalid_thread,
                     "RTC control cannot be called from a callback, worker or nested session");
    }
    lock_.lock();
    active_session = &session;
  }

  ~OperationGuard() {
    active_session = nullptr;
  }

  OperationGuard(const OperationGuard&) = delete;
  OperationGuard& operator=(const OperationGuard&) = delete;

 private:
  std::unique_lock<std::mutex> lock_;
};

template <class T>
Result<T> failure_result(ErrorCode code, std::string message) {
  return Result<T>::failure(std::make_shared<FailureInfo>(
      FailureInfo{next_failure_id(), ErrorDomain::lifecycle, code, std::move(message)}));
}

template <class Function>
auto control_boundary(Function&& function) {
  using Value = std::invoke_result_t<Function>;
  try {
    if constexpr (std::is_void_v<Value>) {
      function();
      return ErrorCode::ok;
    } else {
      return function();
    }
  } catch (...) {
    control_failure = describe_failure(std::current_exception());
    if constexpr (std::is_void_v<Value> || std::is_same_v<Value, ErrorCode>) {
      return describe_failure(std::current_exception())->code;
    } else {
      return Value::failure(describe_failure(std::current_exception()));
    }
  }
}
}  // namespace livekit::sdk::rtc::detail
