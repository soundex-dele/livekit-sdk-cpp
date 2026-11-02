#include "livekit/client/track_publication.hpp"
#include "backends/livekit/participant_state.hpp"
#include "support/logger.h"

namespace livekit::sdk::client::detail {
void PublicationData::emit(RoomEventKind kind, Failure failure, TrackSid previous) {
  RoomEvent event;
  event.kind = kind;
  event.failure = std::move(failure);
  event.previous_sid = std::move(previous);
  {
    std::lock_guard lock(mutex);
    event.publication = info;
    event.participant_identity = info.participant_identity;
  }
  if (auto e = engine.lock()) {
    event.epoch = e->epoch();
  }
  if (notify) {
    notify(std::move(event));
  }
}

Result<void> PublicationData::subscribe(RtcSession& session, const Operation& op, bool enabled) {
  TrackSid sid;
  {
    std::lock_guard lock(mutex);
    if (info.state == TrackState::closed) {
      return Result<void>::failure(
          make_failure(ErrorDomain::media, ErrorCode::stale_handle, "Publication removed"));
    }
    info.desired = enabled;
    sid = info.sid;
  }
  auto result = session.subscribe(sid, track, enabled, op);
  if (result && enabled) {
    result = session.receive_options(sid, receive);
  }
  bool removed;
  {
    std::lock_guard lock(mutex);
    removed = info.state == TrackState::closed;
  }
  if (removed) {
    auto cleanup = session.subscribe(sid, track, false, op);
    if (!cleanup) {
      LOG_ERROR("LiveKitClient", "Removed subscription cleanup: " << cleanup.error().message);
    }
    track->transition(TrackState::closed);
    return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::stale_handle,
                                              "Publication disappeared while subscribing"));
  }
  {
    std::lock_guard lock(mutex);
    info.subscribed = enabled && bool(result);
    info.state = !result ? TrackState::failed : enabled ? TrackState::ready : TrackState::pending;
    info.failure = result ? Failure{} : result.failure_record();
  }
  emit(!result   ? RoomEventKind::track_failed
       : enabled ? RoomEventKind::track_subscribed
                 : RoomEventKind::track_unsubscribed,
       result ? Failure{} : result.failure_record());
  return result;
}
}  // namespace livekit::sdk::client::detail

namespace livekit::sdk::client {
PublicationSnapshot LocalTrackPublication::snapshot() const {
  if (!data_) {
    return {};
  }
  std::lock_guard lock(data_->mutex);
  auto info = data_->info;
  if (data_->track) {
    info.muted = data_->track->snapshot().muted;
  }
  return info;
}

PublicationSnapshot RemoteTrackPublication::snapshot() const {
  if (!data_) {
    return {};
  }
  std::lock_guard lock(data_->mutex);
  return data_->info;
}

LocalTrack LocalTrackPublication::track() const {
  if (!data_) {
    return {};
  }
  std::lock_guard lock(data_->mutex);
  return LocalTrack(data_->track);
}

std::optional<RemoteTrack> RemoteTrackPublication::track() const {
  if (!data_) {
    return {};
  }
  std::lock_guard lock(data_->mutex);
  return data_->info.subscribed ? std::optional<RemoteTrack>(RemoteTrack(data_->track))
                                : std::nullopt;
}

void RemoteTrackPublication::set_subscribed(bool enabled) {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    LOG_ERROR("LiveKitClient", "set_subscribed: detached publication");
    return;
  }
  auto result =
      engine->perform([data = data_, enabled](detail::RtcSession& s, const detail::Operation& op) {
        return data->subscribe(s, op, enabled);
      });
  if (!result) {
    LOG_ERROR("LiveKitClient", "set_subscribed: " << result.error().message);
  }
}

void RemoteTrackPublication::set_receive_options(ReceiveOptions options) {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    LOG_ERROR("LiveKitClient", "set_receive_options: detached publication");
    return;
  }
  auto result =
      engine->perform([data = data_, options](detail::RtcSession& s, const detail::Operation&) {
        if ((options.width == 0) != (options.height == 0) || int(options.quality) < 0 ||
            int(options.quality) > 2) {
          return Result<void>::failure(make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                                                    "Invalid receive dimensions or quality"));
        }
        TrackSid sid;
        {
          std::lock_guard lock(data->mutex);
          if (data->info.state == TrackState::closed) {
            return Result<void>::failure(
                make_failure(ErrorDomain::media, ErrorCode::stale_handle, "Publication removed"));
          }
          sid = data->info.sid;
        }
        if (auto result = s.receive_options(sid, options); !result) {
          return result;
        }
        {
          std::lock_guard lock(data->mutex);
          data->receive = options;
        }
        {
          std::lock_guard lock(data->track->mutex);
          data->track->info.receive_demand = options;
        }
        return Result<void>::success();
      });
  if (!result) {
    LOG_ERROR("LiveKitClient", "set_receive_options: " << result.error().message);
  }
}
}  // namespace livekit::sdk::client
