#pragma once
#include "livekit/result.hpp"
#include <atomic>
#include <future>
#include <type_traits>
#include <utility>

namespace livekit::sdk::client::detail {
// Service queues own completion; abandoned work settles explicitly on teardown.
// get_future is called exactly once before the source is shared with workers.
template <class T>
class ServicePromise {
 public:
  ~ServicePromise() {
    if (!settled_.load()) {
      set_failure(make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled,
                               "Service operation abandoned before completion"));
    }
  }

  std::future<Result<T>> get_future() {
    return promise_.get_future();
  }

  template <class... Args>
  void set_value(Args&&... args) {
    if (!settled_.exchange(true)) {
      promise_.set_value(Result<T>::success(std::forward<Args>(args)...));
    }
  }

  void set_failure(Failure failure) {
    if (!settled_.exchange(true)) {
      promise_.set_value(Result<T>::failure(std::move(failure)));
    }
  }

 private:
  std::promise<Result<T>> promise_;
  std::atomic<bool> settled_{false};
};

template <class T>
std::future<Result<T>> failed_service(Failure failure) {
  ServicePromise<T> promise;
  auto result = promise.get_future();
  promise.set_failure(std::move(failure));
  return result;
}
}  // namespace livekit::sdk::client::detail
