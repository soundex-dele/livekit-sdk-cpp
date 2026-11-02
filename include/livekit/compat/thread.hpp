#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace livekit::compat {
namespace detail {
struct StopCallback {
  std::function<void()> invoke;
  std::thread::id running;
};

struct StopState {
  std::atomic<bool> requested{false};
  std::atomic<unsigned> sources{1};
  std::mutex mutex;
  std::condition_variable completed;
  std::vector<std::shared_ptr<StopCallback>> callbacks;
};
}  // namespace detail

class stop_source;
template <class Callback>
class stop_callback;

class stop_token {
 public:
  stop_token() noexcept = default;

  bool stop_requested() const noexcept {
    return state_ && state_->requested.load();
  }

  bool stop_possible() const noexcept {
    return state_ && (state_->sources.load() != 0 || state_->requested.load());
  }

  friend bool operator==(const stop_token& a, const stop_token& b) noexcept {
    return a.state_ == b.state_;
  }

  friend bool operator!=(const stop_token& a, const stop_token& b) noexcept {
    return !(a == b);
  }

 private:
  friend class stop_source;
  template <class Callback>
  friend class stop_callback;

  explicit stop_token(std::shared_ptr<detail::StopState> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::StopState> state_;
};

struct nostopstate_t {};

inline constexpr nostopstate_t nostopstate{};

class stop_source {
 public:
  stop_source() : state_(std::make_shared<detail::StopState>()) {}

  explicit stop_source(nostopstate_t) noexcept {}

  stop_source(const stop_source& other) noexcept : state_(other.state_) {
    if (state_) {
      ++state_->sources;
    }
  }

  stop_source(stop_source&&) noexcept = default;

  stop_source& operator=(const stop_source& other) noexcept {
    stop_source copy(other);
    swap(copy);
    return *this;
  }

  stop_source& operator=(stop_source&& other) noexcept {
    stop_source moved(std::move(other));
    swap(moved);
    return *this;
  }

  ~stop_source() {
    if (state_) {
      --state_->sources;
    }
  }

  void swap(stop_source& other) noexcept {
    state_.swap(other.state_);
  }

  stop_token get_token() const noexcept {
    return stop_token(state_);
  }

  bool stop_possible() const noexcept {
    return bool(state_);
  }

  bool stop_requested() const noexcept {
    return get_token().stop_requested();
  }

  // Callbacks run synchronously, outside the registry lock. A callback may
  // unregister itself or another callback, or destroy the requesting source.
  bool request_stop() noexcept {
    auto state = state_;
    if (!state) {
      return false;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->requested.exchange(true)) {
      return false;
    }
    while (!state->callbacks.empty()) {
      auto callback = std::move(state->callbacks.back());
      state->callbacks.pop_back();
      callback->running = std::this_thread::get_id();
      lock.unlock();
      callback->invoke();
      lock.lock();
      callback->running = {};
      state->completed.notify_all();
    }
    return true;
  }

 private:
  std::shared_ptr<detail::StopState> state_;
};

template <class Callback>
class stop_callback {
 public:
  template <class F>
  explicit stop_callback(const stop_token& token, F&& function) : state_(token.state_) {
    auto callable = std::make_shared<Callback>(std::forward<F>(function));
    callback_ = std::make_shared<detail::StopCallback>();
    callback_->invoke = [callable] { std::invoke(*callable); };
    if (!state_) {
      return;
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->requested.load()) {
      lock.unlock();
      callback_->invoke();
    } else if (state_->sources.load() != 0) {
      state_->callbacks.push_back(callback_);
    }
  }

  stop_callback(const stop_callback&) = delete;
  stop_callback& operator=(const stop_callback&) = delete;

  ~stop_callback() {
    if (!state_) {
      return;
    }
    std::unique_lock<std::mutex> lock(state_->mutex);
    auto& callbacks = state_->callbacks;
    callbacks.erase(std::remove(callbacks.begin(), callbacks.end(), callback_), callbacks.end());
    // Destruction from another thread waits until its callback has returned.
    // Self-destruction is safe: request_stop retains the callback and callable.
    state_->completed.wait(lock, [&] {
      return callback_->running == std::thread::id{} ||
             callback_->running == std::this_thread::get_id();
    });
  }

 private:
  std::shared_ptr<detail::StopState> state_;
  std::shared_ptr<detail::StopCallback> callback_;
};
template <class Callback>
stop_callback(stop_token, Callback) -> stop_callback<Callback>;

class jthread {
 public:
  using id = std::thread::id;
  using native_handle_type = std::thread::native_handle_type;

  jthread() noexcept : source_(nostopstate) {}

  template <class F, class... Args,
            std::enable_if_t<!std::is_same_v<std::decay_t<F>, jthread>, int> = 0>
  explicit jthread(F&& function, Args&&... args) {
    if constexpr (std::is_invocable_v<std::decay_t<F>, stop_token, std::decay_t<Args>...>) {
      thread_ =
          std::thread(std::forward<F>(function), source_.get_token(), std::forward<Args>(args)...);
    } else {
      thread_ = std::thread(std::forward<F>(function), std::forward<Args>(args)...);
    }
  }

  jthread(jthread&&) noexcept = default;

  jthread& operator=(jthread&& other) noexcept {
    if (this != &other) {
      stop_and_join();
      source_ = std::move(other.source_);
      thread_ = std::move(other.thread_);
    }
    return *this;
  }

  ~jthread() {
    stop_and_join();
  }

  bool joinable() const noexcept {
    return thread_.joinable();
  }

  void join() {
    thread_.join();
  }

  void detach() {
    thread_.detach();
  }

  id get_id() const noexcept {
    return thread_.get_id();
  }

  native_handle_type native_handle() {
    return thread_.native_handle();
  }

  stop_token get_stop_token() const noexcept {
    return source_.get_token();
  }

  stop_source get_stop_source() noexcept {
    return source_;
  }

  bool request_stop() noexcept {
    return source_.request_stop();
  }

  void swap(jthread& other) noexcept {
    source_.swap(other.source_);
    thread_.swap(other.thread_);
  }

  static unsigned hardware_concurrency() noexcept {
    return std::thread::hardware_concurrency();
  }

 private:
  void stop_and_join() {
    if (joinable()) {
      request_stop();
      join();
    }
  }

  stop_source source_;
  std::thread thread_;
};

// The gate serializes notification with unlocking the caller's lock and
// entering the wait. This prevents a stop request from being lost in between.
class condition_variable_any {
 public:
  void notify_one() noexcept {
    std::lock_guard<std::mutex> lock(gate_);
    changed_.notify_one();
  }

  void notify_all() noexcept {
    std::lock_guard<std::mutex> lock(gate_);
    changed_.notify_all();
  }

  template <class Lock>
  void wait(Lock& lock) {
    changed_.wait(lock);
  }

  template <class Lock, class Predicate>
  void wait(Lock& lock, Predicate predicate) {
    changed_.wait(lock, std::move(predicate));
  }

  template <class Lock, class Rep, class Period>
  std::cv_status wait_for(Lock& lock, std::chrono::duration<Rep, Period> duration) {
    return changed_.wait_for(lock, duration);
  }

  template <class Lock, class Rep, class Period, class Predicate>
  bool wait_for(Lock& lock, std::chrono::duration<Rep, Period> duration, Predicate predicate) {
    return changed_.wait_for(lock, duration, std::move(predicate));
  }

  template <class Lock, class Clock, class Duration, class Predicate>
  bool wait_until(Lock& lock, std::chrono::time_point<Clock, Duration> deadline,
                  Predicate predicate) {
    return changed_.wait_until(lock, deadline, std::move(predicate));
  }

  template <class Lock, class Predicate>
  bool wait(Lock& lock, stop_token stop, Predicate predicate) {
    stop_callback callback(stop, [this] { notify_all(); });
    while (!predicate()) {
      std::unique_lock<std::mutex> gate(gate_);
      if (stop.stop_requested()) {
        gate.unlock();
        return predicate();
      }
      lock.unlock();
      changed_.wait(gate);
      gate.unlock();
      lock.lock();
    }
    return true;
  }

  template <class Lock, class Rep, class Period, class Predicate>
  bool wait_for(Lock& lock, stop_token stop, std::chrono::duration<Rep, Period> duration,
                Predicate predicate) {
    return wait_until(lock, stop, std::chrono::steady_clock::now() + duration,
                      std::move(predicate));
  }

  template <class Lock, class Clock, class Duration, class Predicate>
  bool wait_until(Lock& lock, stop_token stop, std::chrono::time_point<Clock, Duration> deadline,
                  Predicate predicate) {
    stop_callback callback(stop, [this] { notify_all(); });
    while (!predicate()) {
      std::unique_lock<std::mutex> gate(gate_);
      if (stop.stop_requested()) {
        gate.unlock();
        return predicate();
      }
      lock.unlock();
      const auto status = changed_.wait_until(gate, deadline);
      gate.unlock();
      lock.lock();
      if (status == std::cv_status::timeout) {
        return predicate();
      }
    }
    return true;
  }

 private:
  std::mutex gate_;
  std::condition_variable_any changed_;
};

}  // namespace livekit::compat
