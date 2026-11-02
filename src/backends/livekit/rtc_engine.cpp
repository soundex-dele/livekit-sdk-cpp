#include "backends/livekit/rtc_engine.hpp"
#include "support/logger.h"
#include <algorithm>

namespace livekit::sdk::client::detail {
void initialize_logging() noexcept;

RtcEngine::RtcEngine(SessionFactory factory) : factory_(std::move(factory)) {
  initialize_logging();
  worker_ = std::thread([this] { run(); });
}

RtcEngine::~RtcEngine() {
  close();
}

void RtcEngine::state(ConnectionState value, Failure error) {
  connected_ = value == ConnectionState::connected;
  if (state_changed) {
    state_changed(value, std::move(error));
  }
}

Result<void> RtcEngine::connect(ConnectionOptions options) {
  const auto duration = std::chrono::seconds(std::max(1, options.connect_timeout));
  return invoke<void>(
      [this, options = std::move(options)](const Operation& op) mutable {
        if (started_) {
          return Result<void>::failure(make_failure(ErrorDomain::lifecycle,
                                                    ErrorCode::invalid_state,
                                                    "Room connect may only be called once"));
        }
        started_ = true;
        options_ = std::move(options);
        {
          std::lock_guard lock(mutex_);
          max_events_ = options_.max_events;
        }
        timeout_ms_ = options_.operation_timeout.count();
        state(ConnectionState::connecting);
        session_ = factory_ ? factory_() : nullptr;
        auto result = Result<void>::failure(make_failure(
            ErrorDomain::transport, ErrorCode::unsupported, "Native client backend is not built"));
        if (session_) {
          session_->events = [this](SessionEvent e) { on_event(std::move(e)); };
          auto join = session_->connect(options_, epoch_, op);
          result = join ? Result<void>::success() : Result<void>::failure(join.failure_record());
          if (join && joined) {
            result = joined(*join, false, *session_, op);
          }
        }
        if (!result) {
          if (session_) {
            session_->close();
          }
          state(ConnectionState::failed, result.failure_record());
          return result;
        }
        state(ConnectionState::connected);
        return Result<void>::success();
      },
      duration);
}

void RtcEngine::close() {
  if (sdk_thread) {
    LOG_ERROR("LiveKitClient", "close is not allowed on an SDK callback thread");
    return;
  }
  std::lock_guard serial(close_mutex_);
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void RtcEngine::recover(Failure error) {
  if (!options_.reconnect.enabled || (error && !error->retryable)) {
    state(ConnectionState::failed, error);
    session_->close();
    return;
  }
  state(ConnectionState::reconnecting, error);
  session_->close();
  const auto deadline = SteadyClock::now() + options_.reconnect.timeout;
  auto delay = options_.reconnect.initial_delay;
  for (size_t attempt = 0;
       options_.reconnect.enabled && attempt < options_.reconnect.max_attempts && !stopped_;
       ++attempt) {
    {
      std::unique_lock lock(mutex_);
      changed_.wait_until(lock, std::min(deadline, SteadyClock::now() + delay),
                          [&] { return stopped_.load(); });
    }
    Operation op{
        std::min(deadline, SteadyClock::now() + std::chrono::seconds(options_.connect_timeout)),
        &stopped_};
    if (!op.status()) {
      break;
    }
    ++epoch_;
    session_ = factory_();
    if (!session_) {
      break;
    }
    session_->events = [this](SessionEvent e) { on_event(std::move(e)); };
    auto join = session_->connect(options_, epoch_, op);
    error = join ? Failure{} : join.failure_record();
    if (join && joined) {
      auto restored = joined(*join, true, *session_, op);
      if (!restored) {
        error = restored.failure_record();
      }
    }
    if (!error) {
      state(ConnectionState::connected);
      return;
    }
    state(ConnectionState::reconnecting, error);
    session_->close();
    delay = std::min(delay * 2, options_.reconnect.max_delay);
  }
  if (!stopped_) {
    state(ConnectionState::failed, error);
  }
}

void RtcEngine::run() {
  sdk_thread = true;
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      changed_.wait_for(lock, std::chrono::milliseconds(5),
                        [&] { return stopped_ || !commands_.empty(); });
      if (!commands_.empty()) {
        task = std::move(commands_.front());
        commands_.pop_front();
      } else if (stopped_) {
        break;
      }
    }
    if (task) {
      task();
    }
    if (connected_ && !stopped_) {
      Operation op{SteadyClock::now() + timeout(), &stopped_};
      if (auto result = session_->poll(op); !result) {
        recover(result.failure_record());
      }
    }
  }
  Failure terminal;
  {
    std::lock_guard lock(mutex_);
    terminal = terminal_failure_;
  }
  if (terminal) {
    state(ConnectionState::failed, terminal);
  }
  state(ConnectionState::closing);
  if (session_) {
    session_->close();
    session_.reset();
  }
  state(ConnectionState::closed);
  sdk_thread = false;
}
#ifndef LIVEKIT_CLIENT_NATIVE
std::unique_ptr<RtcSession> create_rtc_session() {
  return {};
}
#endif
}  // namespace livekit::sdk::client::detail
