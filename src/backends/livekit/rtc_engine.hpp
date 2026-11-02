#pragma once
#include "backends/livekit/rtc_session.hpp"
#include <condition_variable>
#include <deque>
#include <thread>

namespace livekit::sdk::client::detail {
inline thread_local bool sdk_thread = false;

class RtcEngine : public std::enable_shared_from_this<RtcEngine> {
 public:
  explicit RtcEngine(SessionFactory);
  ~RtcEngine();
  Result<void> connect(ConnectionOptions);
  void close();

  bool available() const {
    return connected_;
  }

  Epoch epoch() const {
    return epoch_;
  }

  std::chrono::milliseconds timeout() const {
    return std::chrono::milliseconds(timeout_ms_);
  }

  const ConnectionOptions& options() const {
    return options_;
  }

  std::function<void(ConnectionState, Failure)> state_changed;
  std::function<Result<void>(const JoinState&, bool, RtcSession&, const Operation&)> joined;
  RtcSession::Events events;

  void request_stop(Failure error) {
    std::lock_guard lock(mutex_);
    if (!terminal_failure_) {
      terminal_failure_ = std::move(error);
    }
    stopped_ = true;
    changed_.notify_all();
  }

  bool defer(std::function<void(RtcSession&, const Operation&)> function) {
    std::lock_guard lock(mutex_);
    if (stopped_ || commands_.size() >= max_events_) {
      return false;
    }
    commands_.push_back([this, function = std::move(function)] {
      if (session_ && connected_ && !stopped_) {
        Operation op{SteadyClock::now() + timeout(), &stopped_};
        function(*session_, op);
      }
    });
    changed_.notify_all();
    return true;
  }

  template <class T, class F>
  Result<T> invoke(F function, std::chrono::milliseconds timeout = {}) {
    if (sdk_thread) {
      return Result<T>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_thread,
                                             "Synchronous operation on SDK callback thread"));
    }

    struct Pending {
      std::mutex mutex;
      std::condition_variable changed;
      std::optional<Result<T>> result;
      bool running = false, abandoned = false;
    };

    auto pending = std::make_shared<Pending>();
    Operation op{SteadyClock::now() + (timeout.count() > 0 ? timeout : this->timeout()), &stopped_};
    {
      std::lock_guard lock(mutex_);
      if (stopped_ || commands_.size() >= max_events_) {
        return Result<T>::failure(
            make_failure(ErrorDomain::lifecycle,
                         stopped_ ? ErrorCode::session_closed : ErrorCode::resource_exhausted,
                         "Engine closed or command queue full"));
      }
      commands_.push_back([pending, op, function = std::move(function)]() mutable {
        {
          std::lock_guard lock(pending->mutex);
          if (pending->abandoned) {
            return;
          }
          pending->running = true;
        }
        auto status = op.status();
        auto result = !status ? Result<T>::failure(status.failure_record()) : function(op);
        std::lock_guard lock(pending->mutex);
        pending->result.emplace(std::move(result));
        pending->changed.notify_all();
      });
    }
    changed_.notify_all();
    std::unique_lock lock(pending->mutex);
    while (!pending->result) {
      if (!pending->running) {
        if (auto error = op.status(); !error) {
          pending->abandoned = true;
          return Result<T>::failure(error.failure_record());
        }
      }
      pending->changed.wait_for(lock, std::chrono::milliseconds(5));
    }
    return std::move(*pending->result);
  }

  template <class T, class F>
  Result<T> call(F function) {
    if (!connected_) {
      return Result<T>::failure(
          make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state, "Room is not connected"));
    }
    return invoke<T>([this, function = std::move(function)](const Operation& op) mutable {
      if (!connected_ || !session_) {
        return Result<T>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state,
                                               "Room is not connected"));
      }
      return function(*session_, op);
    });
  }

  template <class F>
  Result<void> perform(F function) {
    return call<void>(std::move(function));
  }

 private:
  void run();
  void recover(Failure);

  void on_event(SessionEvent event) {
    if (event.kind == SessionEvent::Kind::token) {
      options_.token = std::move(event.token);
    } else if (events) {
      events(std::move(event));
    }
  }

  void state(ConnectionState, Failure = {});
  SessionFactory factory_;
  std::unique_ptr<RtcSession> session_;
  ConnectionOptions options_;
  std::atomic<bool> stopped_{false}, connected_{false};
  std::atomic<Epoch> epoch_{1};
  std::atomic<int64_t> timeout_ms_{30000};
  size_t max_events_ = 4096;
  std::mutex mutex_, close_mutex_;
  std::condition_variable changed_;
  std::deque<std::function<void()>> commands_;
  std::thread worker_;
  bool started_ = false;
  Failure terminal_failure_;
};
}  // namespace livekit::sdk::client::detail
