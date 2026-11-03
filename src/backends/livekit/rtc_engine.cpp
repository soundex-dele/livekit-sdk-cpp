#include "backends/livekit/rtc_engine.hpp"
#include "support/logger.h"
#include <algorithm>
#include "backends/livekit/region_policy.hpp"
#include "backends/livekit/region_discovery.hpp"

namespace livekit::sdk::client::detail {
void initialize_logging() noexcept;

RtcEngine::RtcEngine(SessionFactory factory)
    : factory_(std::move(factory)), data_services_(create_room_data()) {
  initialize_logging();
  worker_ = std::thread([this] { run(); });
}

RtcEngine::~RtcEngine() {
  close();
}

void RtcEngine::state(ConnectionState value, Failure error) {
  connected_ = value == ConnectionState::connected;
  if (data_services_ && (value == ConnectionState::reconnecting ||
                         value == ConnectionState::failed || value == ConnectionState::closing)) {
    data_services_->interrupt(value == ConnectionState::reconnecting);
  }
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
        original_url_ = options_.url;
        {
          std::lock_guard lock(mutex_);
          max_events_ = options_.max_events;
        }
        timeout_ms_ = options_.operation_timeout.count();
        state(ConnectionState::connecting);
        auto joined_state = join(op, true);
        auto result = joined_state ? Result<void>::success()
                                   : Result<void>::failure(joined_state.failure_record());
        if (joined_state && joined) {
          result = joined(*joined_state, false, *session_, op);
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
  if (sdk_thread || service_handler_thread) {
    LOG_ERROR("LiveKitClient", "close is not allowed on an SDK callback thread");
    return;
  }
  std::lock_guard serial(close_mutex_);
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
    provider_stop_.request_stop();
  }
  changed_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  if (data_services_) {
    data_services_->shutdown();
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
    auto join = this->join(op, false);
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
    if (session_) {
      session_->close();
    }
    delay = std::min(delay * 2, options_.reconnect.max_delay);
  }
  if (!stopped_) {
    state(ConnectionState::failed, error);
  }
}

Result<void> RtcEngine::refresh_token(TokenRequestReason reason, const Operation& op) {
  if (!options_.token_provider) {
    return Result<void>::success();
  }
  auto result =
      options_.token_provider({original_url_, reason, op.deadline, provider_stop_.get_token()});
  if (!result) {
    // Provider errors are application text and may contain credentials.
    return Result<void>::failure(
        make_failure(ErrorDomain::signaling, result.error().code, "Token provider failed"));
  }
  if (auto status = op.status(); !status) {
    return status;
  }
  if (result->empty() || result->size() > 65536) {
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::invalid_argument,
                                              "Token provider returned an invalid token"));
  }
  options_.token = std::move(*result);
  return Result<void>::success();
}

Result<JoinState> RtcEngine::join(const Operation& op, bool initial) {
  using Return = Result<JoinState>;
  auto endpoint = signal_endpoint(original_url_);
  if (!endpoint) {
    return Return::failure(endpoint.failure_record());
  }
  if (initial) {
    auto token = refresh_token(TokenRequestReason::initial, op);
    if (!token) {
      return Return::failure(token.failure_record());
    }
    region_candidates_ = {endpoint->url};
    auto candidates = options_.regions.fallback_endpoints;
    auto discovered = discover_regions(options_, op);
    candidates.insert(candidates.end(), discovered.begin(), discovered.end());
    for (const auto& candidate : candidates) {
      auto trusted = trusted_destination(original_url_, candidate, options_.regions);
      if (trusted && std::find(region_candidates_.begin(), region_candidates_.end(), *trusted) ==
                         region_candidates_.end()) {
        region_candidates_.push_back(*trusted);
      }
      if (region_candidates_.size() >= 32) {
        break;
      }
    }
  }
  bool refreshed = false;
  size_t redirects = 0, attempts = 0;
  Failure failure;
  for (;;) {
    if (auto status = op.status(); !status) {
      return Return::failure(status.failure_record());
    }
    auto trusted = trusted_destination(original_url_, options_.url, options_.regions);
    if (!trusted) {
      return Return::failure(trusted.failure_record());
    }
    options_.url = *trusted;
    session_ = factory_ ? factory_() : nullptr;
    if (!session_) {
      return Return::failure(make_failure(ErrorDomain::transport, ErrorCode::unsupported,
                                          "Native client backend is not built"));
    }
    session_->data_services = data_services_;
    session_->events = [this](SessionEvent e) { on_event(std::move(e)); };
    auto result = session_->connect(options_, epoch_, op);
    if (result) {
      return result;
    }
    failure = result.failure_record();
    session_->close();
    if (failure->backend_code == "region_redirect") {
      if (++redirects > options_.regions.max_redirects) {
        return Return::failure(make_failure(ErrorDomain::signaling, ErrorCode::permission_denied,
                                            "Region redirect limit exceeded"));
      }
      auto redirected = trusted_destination(original_url_, failure->backend_data, options_.regions);
      if (!redirected) {
        return Return::failure(redirected.failure_record());
      }
      options_.url = std::move(*redirected);
      continue;
    }
    if (failure->code == ErrorCode::permission_denied && options_.token_provider && !refreshed) {
      auto token = refresh_token(TokenRequestReason::authentication_failed, op);
      if (!token) {
        return Return::failure(token.failure_record());
      }
      refreshed = true;
      continue;
    }
    if ((initial && !options_.regions.initial_failover) ||
        (!failure->retryable && failure->code != ErrorCode::timeout) ||
        ++attempts >= region_candidates_.size()) {
      return Return::failure(failure);
    }
    region_cursor_ = (region_cursor_ + 1) % region_candidates_.size();
    options_.url = region_candidates_[region_cursor_];
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
std::shared_ptr<RoomData> create_room_data() {
  return {};
}

std::vector<std::string> discover_regions(const ConnectionOptions&, const Operation&) {
  return {};
}

std::unique_ptr<RtcSession> create_rtc_session() {
  return {};
}
#endif
}  // namespace livekit::sdk::client::detail
