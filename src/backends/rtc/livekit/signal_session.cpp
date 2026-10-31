#include "backends/rtc/livekit/signal_session.hpp"
#include "backends/rtc/livekit/reconnect_policy.hpp"
#include "backends/rtc/livekit/token_request.hpp"
#include <algorithm>
#include <random>

namespace lk {
using namespace livekit::sdk;
using Clock = std::chrono::steady_clock;

SignalSession::SignalSession(const SignalingOptions& options, std::stop_token stop,
                             EventEmitter events, detail::SignalRecoveryState* recovery,
                             std::shared_ptr<detail::WakeEvent> wakeup)
    : options_(options),
      stop_(stop),
      events_(std::move(events)),
      socket_(std::move(wakeup)),
      recovery_(recovery ? *recovery : local_recovery_) {
  if (!recovery) {
    recovery_.budget = detail::RecoveryBudget(options.reconnect);
    recovery_.token = options.token;
  }
}

Result<void> SignalSession::connect() {
  auto endpoint = detail::signal_endpoint(options_.url);
  if (!endpoint) {
    return Result<void>::failure(endpoint.failure_record());
  }
  if (recovery_.regions.active_url.empty()) {
    recovery_.regions.active_url = endpoint->url;
  }
  connect_deadline_ =
      std::min({recovery_.budget.deadline(), recovery_.session_deadline,
                Clock::now() + std::chrono::seconds(options_.connect_timeout),
                recovery_.ever_connected ? Clock::time_point::max() : recovery_.initial_deadline});
  auto& regions = recovery_.regions;
  initial_failover_ =
      !recovery_.ever_connected && !recovery_.rejoining && options_.regions.initial_failover;
  if (recovery_.rejoining || initial_failover_) {
    if (options_.regions.discovery && endpoint->cloud && Clock::now() >= regions.expires) {
      if (recovery_.token.empty() && options_.token_provider) {
        auto token = detail::request_token(
            options_.token_provider, regions.active_url, TokenRequestReason::initial,
            std::min(connect_deadline_, Clock::now() + options_.token_timeout), stop_);
        if (!token) {
          return Result<void>::failure(token.failure_record());
        }
        recovery_.token = std::move(*token);
      }
      auto urls = fetch_regions(
          options_.url, recovery_.token, stop_,
          std::min(connect_deadline_, Clock::now() + options_.regions.discovery_timeout));
      if (urls) {
        regions.replace(options_.url, options_.regions, *urls);
      } else if (urls.error().code == ErrorCode::cancelled) {
        return Result<void>::failure(urls.failure_record());
      }
      // Discovery is best effort; retain a previously trusted list on failure.
    }
    if (initial_failover_) {
      auto candidates = regions.candidates;
      candidates.insert(candidates.end(), options_.regions.fallback_endpoints.begin(),
                        options_.regions.fallback_endpoints.end());
      regions.replace(options_.url, options_.regions, candidates);
      regions.attempted.insert(regions.active_url);
    } else
      regions.select();
  }
  redirects_ = 0;
  redirect_history_.clear();
  redirect_history_.insert(regions.active_url);
  return open_initial_socket();
}

bool SignalSession::select_initial_fallback(const FailureInfo& error) {
  if (!initial_failover_ || stop_.stop_requested() || Clock::now() >= connect_deadline_ ||
      error.domain != ErrorDomain::signaling || !error.retryable)
    return false;
  auto& regions = recovery_.regions;
  regions.attempted.insert(regions.active_url);
  const auto previous = regions.active_url;
  regions.select();
  if (regions.active_url == previous)
    return false;
  socket_.close();
  events_("initial-region-failover",
          {{"endpoint", regions.active_url}, {"attempt", regions.attempted.size()}});
  return true;
}

Result<void> SignalSession::open_initial_socket() {
  for (;;) {
    const bool has_alternative =
        initial_failover_ &&
        std::any_of(recovery_.regions.candidates.begin(), recovery_.regions.candidates.end(),
                    [&](const auto& candidate) {
                      return !recovery_.regions.attempted.contains(candidate);
                    });
    attempt_deadline_ = has_alternative ? std::min(connect_deadline_,
                                                   Clock::now() + options_.regions.endpoint_timeout)
                                        : connect_deadline_;
    auto opened = connect_socket({}, attempt_deadline_);
    if (opened || !select_initial_fallback(opened.error())) {
      return opened;
    }
  }
}

Result<bool> SignalSession::failover(const FailureInfo& error) {
  if (!select_initial_fallback(error)) {
    return Result<bool>::success(false);
  }
  auto opened = open_initial_socket();
  if (!opened) {
    return Result<bool>::failure(opened.failure_record());
  }
  return Result<bool>::success(true);
}

Result<void> SignalSession::redirect(const std::string& url) {
  auto trusted = detail::trusted_destination(options_.url, url, options_.regions);
  if (!trusted) {
    return Result<void>::failure(trusted.failure_record());
  }
  auto from = detail::signal_endpoint(recovery_.regions.active_url);
  auto to = detail::signal_endpoint(*trusted);
  if (!from) {
    return Result<void>::failure(from.failure_record());
  }
  if (!to) {
    return Result<void>::failure(to.failure_record());
  }
  if (from->secure && !to->secure)
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::permission_denied,
                                              "Signaling redirect would downgrade TLS"));
  if (++redirects_ > options_.regions.max_redirects || !redirect_history_.insert(*trusted).second)
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::recovery_failed,
                                              "Signaling redirect limit or cycle"));
  recovery_.regions.active_url = std::move(*trusted);
  recovery_.regions.attempted.insert(recovery_.regions.active_url);
  return open_initial_socket();
}

Result<void> SignalSession::update_regions(const livekit::RegionSettings& settings) {
  if (settings.regions_size() > 32)
    return Result<void>::failure(make_failure(ErrorDomain::protocol, ErrorCode::resource_exhausted,
                                              "Too many server region candidates"));
  std::vector<std::string> urls;
  for (const auto& region : settings.regions())
    urls.push_back(region.url());
  recovery_.regions.replace(options_.url, options_.regions, urls);
  return Result<void>::success();
}

Result<void> SignalSession::connect_socket(const std::string& sid, Clock::time_point deadline) {
  auto refresh = [&](TokenRequestReason reason) -> Result<void> {
    auto token =
        detail::request_token(options_.token_provider, recovery_.regions.active_url, reason,
                              std::min(deadline, Clock::now() + options_.token_timeout), stop_);
    if (!token) {
      return Result<void>::failure(token.failure_record());
    }
    recovery_.token = std::move(*token);
    return Result<void>::success();
  };
  if (recovery_.token.empty() && options_.token_provider) {
    auto refreshed = refresh(TokenRequestReason::initial);
    if (!refreshed) {
      return refreshed;
    }
  }
  auto open = [&] {
    std::string wrapped_join;
    if (options_.single_peer_connection) {
      livekit::JoinRequest request;
      request.mutable_client_info()->set_sdk(livekit::ClientInfo::CPP);
      request.mutable_client_info()->set_version("0.3.0");
      request.mutable_client_info()->set_protocol(options_.signal_protocol);
      request.mutable_connection_settings()->set_auto_subscribe(options_.subscribe &&
                                                                options_.subscribe_tracks.empty());
      request.mutable_connection_settings()->set_auto_subscribe_data_track(false);
      if (sid.empty()) {
        *request.mutable_publisher_offer() = initial_offer_;
        for (const auto& track : initial_tracks_)
          *request.add_add_track_requests() = track;
      } else {
        request.set_reconnect(true);
        request.set_participant_sid(sid);
      }
      livekit::WrappedJoinRequest wrapped;
      wrapped.set_join_request(request.SerializeAsString());
      wrapped_join = base64url(wrapped.SerializeAsString());
      while (wrapped_join.size() % 4)
        wrapped_join += '=';
    }
    return socket_.connect(recovery_.regions.active_url, recovery_.token,
                           options_.subscribe && options_.subscribe_tracks.empty(), stop_,
                           options_.connect_timeout, sid, deadline, options_.signal_protocol,
                           wrapped_join);
  };
  auto opened = open();
  if (opened || opened.error().code != ErrorCode::permission_denied ||
      opened.error().backend_reason != 401 || !options_.token_provider) {
    return opened;
  }
  auto refreshed = refresh(TokenRequestReason::authentication_failed);
  if (!refreshed) {
    return refreshed;
  }
  return open();  // Exactly one authenticated retry, sharing the original deadline.
}

void SignalSession::bind_participant(std::string sid) {
  sid_ = std::move(sid);
  initial_failover_ = false;
}

void SignalSession::update_token(std::string token) {
  if (!token.empty())
    recovery_.token = std::move(token);
}

Result<void> SignalSession::send(const livekit::SignalRequest& request) {
  return socket_.send(request.SerializeAsString());
}

void SignalSession::enqueue_ice(const livekit::SignalRequest& request) noexcept {
  auto bytes = request.SerializeAsString();
  std::lock_guard lock(ice_mutex_);
  if (ice_.size() >= 1024) {
    ice_overflow_ = true;
    return;
  }
  ice_.push_back(std::move(bytes));
}

Result<void> SignalSession::flush_ice() {
  std::lock_guard lock(ice_mutex_);
  if (ice_overflow_)
    return Result<void>::failure(make_failure(ErrorDomain::signaling, ErrorCode::resource_exhausted,
                                              "Pending ICE queue overflow"));
  while (!ice_.empty()) {
    auto sent = socket_.send(ice_.front());
    if (!sent) {
      return sent;
    }
    ice_.pop_front();
  }
  return Result<void>::success();
}

Result<std::optional<std::string>> SignalSession::receive(int timeout_ms) {
  return socket_.receive(timeout_ms);
}

void SignalSession::close() {
  socket_.close();
}

Result<bool> SignalSession::recover(const SyncBuilder& sync, const ReconnectHandler& configure,
                                    const MovedHandler& moved) {
  const auto& policy = options_.reconnect;
  if (!policy.enabled || sid_.empty()) {
    return Result<bool>::success(false);
  }
  if (!resume_allowed_) {
    return Result<bool>::failure(
        detail::rejoin_required("Server configuration disables session resume"));
  }
  recovery_.budget.begin();
  const auto deadline = std::min(recovery_.budget.deadline(), recovery_.session_deadline);
  std::mt19937 random(std::random_device{}());
  std::uniform_real_distribution<double> unit(0, 1);
  for (size_t resume_count = 0; resume_count < policy.resume_attempts && !stop_.stop_requested();
       ++resume_count) {
    auto attempt = recovery_.budget.next_attempt();
    if (!attempt) {
      return Result<bool>::failure(attempt.failure_record());
    }
    events_("reconnecting", {{"mode", "resume"}, {"attempt", *attempt}});
    events_.dispatch();
    if (auto status = events_.status(); !status) {
      return Result<bool>::failure(status.failure_record());
    }
    const auto wake =
        std::min(deadline, Clock::now() + detail::reconnect_delay(policy, *attempt, unit(random)));
    if (!detail::wait_reconnect(stop_, wake)) {
      return Result<bool>::success(false);
    }
    if (auto budget = recovery_.budget.check(); !budget) {
      return Result<bool>::failure(budget.failure_record());
    }
    const auto attempt_deadline =
        std::min(deadline, Clock::now() + std::chrono::seconds(options_.connect_timeout));
    auto resume = [&]() -> Result<bool> {
      auto connected = connect_socket(sid_, attempt_deadline);
      if (!connected) {
        return Result<bool>::failure(connected.failure_record());
      }
      while (!stop_.stop_requested() && Clock::now() < attempt_deadline) {
        events_.dispatch();
        if (auto status = events_.status(); !status) {
          return Result<bool>::failure(status.failure_record());
        }
        auto received = socket_.receive_until(attempt_deadline, stop_);
        if (!received) {
          return Result<bool>::failure(received.failure_record());
        }
        if (!*received) {
          continue;
        }
        livekit::SignalResponse response;
        if (!response.ParseFromString(**received)) {
          return Result<bool>::failure(make_failure(
              ErrorDomain::protocol, ErrorCode::malformed_message, "Malformed resume response"));
        }
        if (response.has_refresh_token()) {
          update_token(response.refresh_token());
          continue;
        }
        if (response.has_room_moved() && moved) {
          moved(response.room_moved());
          continue;
        }
        if (response.has_leave()) {
          if (!detail::terminal_leave(response.leave().reason()) &&
              (response.leave().action() != 0 || response.leave().can_reconnect() ||
               response.leave().reason() == 6)) {
            if (response.leave().has_regions()) {
              auto updated = update_regions(response.leave().regions());
              if (!updated) {
                return Result<bool>::failure(updated.failure_record());
              }
            }
            return Result<bool>::success(false);  // Explicit fresh-join request.
          }
          events_("server-leave", {{"reason", response.leave().reason()}});
          events_.dispatch();
          return Result<bool>::failure(
              make_failure(ErrorDomain::signaling, ErrorCode::recovery_failed,
                           "Server rejected signal resume", false, response.leave().reason()));
        }
        if (!response.has_reconnect()) {
          return Result<bool>::success(false);
        }
        configure(response.reconnect());
        livekit::SignalRequest request;
        sync(*request.mutable_sync_state());
        if (auto sent = send(request); !sent) {
          return Result<bool>::failure(sent.failure_record());
        }
        if (auto sent = flush_ice(); !sent) {
          return Result<bool>::failure(sent.failure_record());
        }
        recovery_attempt_ = *attempt;
        return Result<bool>::success(true);
      }
      return Result<bool>::failure(make_failure(ErrorDomain::signaling, ErrorCode::timeout,
                                                "Signal resume timed out", true));
    }();
    if (resume && *resume) {
      return resume;
    }
    socket_.close();
    if (resume) {
      return Result<bool>::failure(detail::rejoin_required("Server requested a fresh join"));
    }
    if (!resume.error().retryable) {
      return resume;
    }
  }
  if (stop_.stop_requested()) {
    return Result<bool>::success(false);
  }
  return Result<bool>::failure(
      detail::rejoin_required("Signal resume unsuccessful; fresh join required"));
}
}  // namespace lk
