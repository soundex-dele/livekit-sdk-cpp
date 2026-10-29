#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace livekit::sdk {
template <class T>
class AsyncResult;
template <class T>
class CompletionSource;

namespace async_detail {
struct Completion {
  std::mutex mutex;
  bool ready = false;
  std::vector<std::function<void()>> continuations;

  void subscribe(std::function<void()> continuation) {
    {
      std::lock_guard lock(mutex);
      if (!ready) {
        continuations.push_back(std::move(continuation));
        return;
      }
    }
    continuation();
  }

  void finish() noexcept {
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard lock(mutex);
      if (ready) {
        return;
      }
      ready = true;
      callbacks.swap(continuations);
    }
    for (auto& callback : callbacks) {
      // Only AsyncResult::then installs callbacks; each captures both dispatch
      // and execution exceptions in its own completion source.
      callback();
    }
  }
};

template <class T>
struct Unwrap {
  using Type = T;
  static constexpr bool asynchronous = false;
};

template <class T>
struct Unwrap<AsyncResult<T>> {
  using Type = T;
  static constexpr bool asynchronous = true;
};
}  // namespace async_detail

// Move-only operation result. Waiting is optional: then() schedules exactly one
// continuation when the operation settles, without a timer or a waiting thread.
// Dispatch must enqueue the supplied callable or throw; it must not silently
// discard accepted work. Continuations receive a ready result, including errors.
template <class T>
class AsyncResult {
 public:
  AsyncResult() = default;
  AsyncResult(AsyncResult&&) noexcept = default;
  AsyncResult& operator=(AsyncResult&&) noexcept = default;
  AsyncResult(const AsyncResult&) = delete;
  AsyncResult& operator=(const AsyncResult&) = delete;

  bool valid() const noexcept {
    return future_.valid();
  }

  T get() {
    return future_.get();
  }

  void wait() const {
    future_.wait();
  }

  template <class Rep, class Period>
  std::future_status wait_for(std::chrono::duration<Rep, Period> timeout) const {
    return future_.wait_for(timeout);
  }

  template <class Clock, class Duration>
  std::future_status wait_until(std::chrono::time_point<Clock, Duration> deadline) const {
    return future_.wait_until(deadline);
  }

  // Explicit interoperability at application boundaries. SDK internals retain
  // AsyncResult so they never need to periodically inspect a standard future.
  std::future<T> as_future() && {
    return std::move(future_);
  }

  template <class Dispatch, class Function>
  auto then(Dispatch dispatch, Function function) && -> AsyncResult<
      typename async_detail::Unwrap<std::invoke_result_t<Function, AsyncResult<T>>>::Type>;

 private:
  friend class CompletionSource<T>;

  AsyncResult(std::future<T> future, std::shared_ptr<async_detail::Completion> completion)
      : future_(std::move(future)), completion_(std::move(completion)) {}

  std::future<T> future_;
  std::shared_ptr<async_detail::Completion> completion_;
};

// A source has a single result. Destruction settles an unfinished result with
// broken_promise and wakes continuations just like explicit failure does.
template <class T>
class CompletionSource {
 public:
  CompletionSource() = default;
  CompletionSource(const CompletionSource&) = delete;
  CompletionSource& operator=(const CompletionSource&) = delete;

  ~CompletionSource() {
    promise_.reset();
    completion_->finish();
  }

  AsyncResult<T> get_result() {
    return AsyncResult<T>(promise_->get_future(), completion_);
  }

  template <class... Args>
  void set_value(Args&&... args) {
    promise_->set_value(std::forward<Args>(args)...);
    completion_->finish();
  }

  void set_exception(std::exception_ptr failure) {
    promise_->set_exception(std::move(failure));
    completion_->finish();
  }

 private:
  std::shared_ptr<async_detail::Completion> completion_ =
      std::make_shared<async_detail::Completion>();
  std::unique_ptr<std::promise<T>> promise_ = std::make_unique<std::promise<T>>();
};

template <class T>
template <class Dispatch, class Function>
auto AsyncResult<T>::then(Dispatch dispatch, Function function) && -> AsyncResult<
    typename async_detail::Unwrap<std::invoke_result_t<Function, AsyncResult<T>>>::Type> {
  using Return = std::invoke_result_t<Function, AsyncResult<T>>;
  using Value = typename async_detail::Unwrap<Return>::Type;
  if (!valid()) {
    throw std::future_error(std::future_errc::no_state);
  }
  auto completion = completion_;
  auto input = std::make_shared<AsyncResult>(std::move(*this));
  auto output = std::make_shared<CompletionSource<Value>>();
  auto result = output->get_result();
  auto invoke = std::make_shared<Function>(std::move(function));
  completion->subscribe([input, output, invoke, dispatch = std::move(dispatch)]() mutable noexcept {
    try {
      dispatch([input, output, invoke] {
        try {
          if constexpr (std::is_void_v<Return>) {
            (*invoke)(std::move(*input));
            output->set_value();
          } else if constexpr (async_detail::Unwrap<Return>::asynchronous) {
            auto next = (*invoke)(std::move(*input));
            std::move(next).then([](auto task) { task(); },
                                 [output](AsyncResult<Value> ready) {
                                   try {
                                     if constexpr (std::is_void_v<Value>) {
                                       ready.get();
                                       output->set_value();
                                     } else {
                                       output->set_value(ready.get());
                                     }
                                   } catch (...) {
                                     output->set_exception(std::current_exception());
                                   }
                                 });
          } else {
            output->set_value((*invoke)(std::move(*input)));
          }
        } catch (...) {
          output->set_exception(std::current_exception());
        }
      });
    } catch (...) {
      output->set_exception(std::current_exception());
    }
  });
  return result;
}
}  // namespace livekit::sdk
