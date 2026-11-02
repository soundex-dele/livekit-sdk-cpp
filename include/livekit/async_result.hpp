#pragma once

#include "livekit/result.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace livekit::sdk {
template <class T>
class AsyncResult;
template <class T>
class CompletionSource;

namespace async_detail {
inline Failure invalid_state() {
  return make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state,
                      "Async result has no unconsumed state", false, 0, {"async_result"});
}

template <class T>
struct State {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<Result<T>> result;
  bool claimed = false;
  bool consumed = false;
  std::vector<std::function<void()>> continuations;

  void subscribe(std::function<void()> continuation) {
    {
      std::lock_guard lock(mutex);
      if (!result) {
        continuations.push_back(std::move(continuation));
        return;
      }
    }
    continuation();
  }

  ErrorCode finish(Result<T> value) {
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard lock(mutex);
      if (result) {
        return ErrorCode::invalid_state;
      }
      result.emplace(std::move(value));
      callbacks.swap(continuations);
    }
    ready.notify_all();
    for (auto& callback : callbacks) {
      callback();
    }
    return ErrorCode::ok;
  }
};

template <class T>
struct Unwrap {
  using Type = T;
  static constexpr bool asynchronous = false;
  static constexpr bool result = false;
};

template <class T>
struct Unwrap<Result<T>> {
  using Type = T;
  static constexpr bool asynchronous = false;
  static constexpr bool result = true;
};
template <class T>
struct Unwrap<AsyncResult<T>> {
  using Type = T;
  static constexpr bool asynchronous = true;
  static constexpr bool result = false;
};
}  // namespace async_detail

// Move-only, single-consumer result. Failures, including an abandoned source or
// an invalid handle, are explicit Result values. Callbacks must not throw.
// Dispatch returns Result<void> (failure means task was not accepted). A void
// dispatcher is supported only for executors that cannot reject submitted work.
template <class T>
class AsyncResult {
 public:
  AsyncResult() = default;
  AsyncResult(AsyncResult&&) noexcept = default;
  AsyncResult& operator=(AsyncResult&&) noexcept = default;
  AsyncResult(const AsyncResult&) = delete;
  AsyncResult& operator=(const AsyncResult&) = delete;

  bool valid() const noexcept {
    return bool(state_);
  }

  Result<T> get() {
    auto state = std::exchange(state_, {});
    if (!state) {
      return Result<T>::failure(async_detail::invalid_state());
    }
    std::unique_lock lock(state->mutex);
    state->ready.wait(lock, [&] { return state->result.has_value(); });
    if (state->consumed) {
      return Result<T>::failure(async_detail::invalid_state());
    }
    state->consumed = true;
    return std::move(*state->result);
  }

  Result<void> wait() const {
    if (!state_) {
      return Result<void>::failure(async_detail::invalid_state());
    }
    std::unique_lock lock(state_->mutex);
    state_->ready.wait(lock, [&] { return state_->result.has_value(); });
    return Result<void>::success();
  }

  // Invalid handles are immediately ready: get() returns invalid_state.
  template <class Rep, class Period>
  std::future_status wait_for(std::chrono::duration<Rep, Period> timeout) const {
    if (!state_) {
      return std::future_status::ready;
    }
    std::unique_lock lock(state_->mutex);
    return state_->ready.wait_for(lock, timeout, [&] { return state_->result.has_value(); })
               ? std::future_status::ready
               : std::future_status::timeout;
  }

  template <class Clock, class Duration>
  std::future_status wait_until(std::chrono::time_point<Clock, Duration> deadline) const {
    if (!state_) {
      return std::future_status::ready;
    }
    std::unique_lock lock(state_->mutex);
    return state_->ready.wait_until(lock, deadline, [&] { return state_->result.has_value(); })
               ? std::future_status::ready
               : std::future_status::timeout;
  }

  std::future<Result<T>> as_future() && {
    auto promise = std::make_shared<std::promise<Result<T>>>();
    auto future = promise->get_future();
    if (!state_) {
      promise->set_value(Result<T>::failure(async_detail::invalid_state()));
      return future;
    }
    auto state = state_;
    auto input = std::make_shared<AsyncResult>(std::move(*this));
    state->subscribe([input, promise] { promise->set_value(input->get()); });
    return future;
  }

  template <class Dispatch, class Function>
  auto then(Dispatch dispatch, Function function) && -> AsyncResult<
      typename async_detail::Unwrap<std::invoke_result_t<Function, AsyncResult<T>>>::Type>;

 private:
  friend class CompletionSource<T>;

  explicit AsyncResult(std::shared_ptr<async_detail::State<T>> state) : state_(std::move(state)) {}

  std::shared_ptr<async_detail::State<T>> state_;
};

// Completion is idempotent: a second completion returns invalid_state and never
// overwrites the original result. Destroying a pending source delivers cancelled.
template <class T>
class CompletionSource {
 public:
  CompletionSource() = default;
  CompletionSource(const CompletionSource&) = delete;
  CompletionSource& operator=(const CompletionSource&) = delete;

  ~CompletionSource() {
    bool pending;
    {
      std::lock_guard lock(state_->mutex);
      pending = !state_->result;
    }
    if (pending) {
      set_failure(make_failure(ErrorDomain::lifecycle, ErrorCode::cancelled,
                               "Async operation abandoned before completion", false, 0,
                               {"async_result"}));
    }
  }

  AsyncResult<T> get_result() {
    std::lock_guard lock(state_->mutex);
    if (state_->claimed) {
      return {};
    }
    state_->claimed = true;
    return AsyncResult<T>(state_);
  }

  template <class... Args>
  ErrorCode set_value(Args&&... args) {
    return set_result(Result<T>::success(std::forward<Args>(args)...));
  }

  ErrorCode set_result(Result<T> result) {
    return state_->finish(std::move(result));
  }

  ErrorCode set_failure(Failure failure) {
    return set_result(Result<T>::failure(std::move(failure)));
  }

 private:
  std::shared_ptr<async_detail::State<T>> state_ = std::make_shared<async_detail::State<T>>();
};

template <class T>
template <class Dispatch, class Function>
auto AsyncResult<T>::then(Dispatch dispatch, Function function) && -> AsyncResult<
    typename async_detail::Unwrap<std::invoke_result_t<Function, AsyncResult<T>>>::Type> {
  using Return = std::invoke_result_t<Function, AsyncResult<T>>;
  using Value = typename async_detail::Unwrap<Return>::Type;
  auto output = std::make_shared<CompletionSource<Value>>();
  auto result = output->get_result();
  if (!state_) {
    output->set_failure(async_detail::invalid_state());
    return result;
  }
  auto state = state_;
  auto input = std::make_shared<AsyncResult>(std::move(*this));
  auto invoke = std::make_shared<Function>(std::move(function));
  state->subscribe([input, output, invoke, dispatch = std::move(dispatch)]() mutable {
    auto task = [input, output, invoke] {
      if constexpr (std::is_void_v<Return>) {
        (*invoke)(std::move(*input));
        output->set_value();
      } else if constexpr (async_detail::Unwrap<Return>::asynchronous) {
        auto next = (*invoke)(std::move(*input));
        if (!next.valid()) {
          output->set_failure(async_detail::invalid_state());
          return;
        }
        std::move(next).then(
            [](auto next_task) { next_task(); },
            [output](AsyncResult<Value> ready) { output->set_result(ready.get()); });
      } else if constexpr (async_detail::Unwrap<Return>::result) {
        output->set_result((*invoke)(std::move(*input)));
      } else {
        output->set_value((*invoke)(std::move(*input)));
      }
    };
    if constexpr (std::is_void_v<std::invoke_result_t<Dispatch&, decltype(task)>>) {
      dispatch(std::move(task));
    } else {
      static_assert(std::is_same_v<std::invoke_result_t<Dispatch&, decltype(task)>, Result<void>>,
                    "Dispatch must return void or Result<void>");
      auto accepted = dispatch(std::move(task));
      if (!accepted) {
        output->set_failure(accepted.failure_record());
      }
    }
  });
  return result;
}
}  // namespace livekit::sdk
