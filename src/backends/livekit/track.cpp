#include "livekit/client/track.hpp"
#include "backends/livekit/track_state.hpp"
#include "support/logger.h"
#include <algorithm>

namespace livekit::sdk::client::detail {
Result<void> validate_track(const TrackConfig& c) {
  bool audio = c.kind == MediaKind::audio;
  if (!c.queue.max_frames || !c.queue.max_bytes || c.queue.max_age.count() <= 0 ||
      c.encodings.size() != 1 || !c.encodings.front().max_bitrate_bps ||
      (audio && (c.format.codec != Codec::opus || c.format.clock_rate != 48000 ||
                 c.format.channels < 1 || c.format.channels > 2)) ||
      (!audio &&
       (c.format.codec == Codec::opus || c.format.clock_rate != 90000 || !c.format.width ||
        !c.format.height || c.format.width > 16384 || c.format.height > 16384))) {
    return Result<void>::failure(
        make_failure(ErrorDomain::media, ErrorCode::invalid_argument,
                     "Invalid encoded format, single encoding, or queue limits"));
  }
  return Result<void>::success();
}

void TrackData::transition(TrackState state, Epoch epoch, TrackSid sid) {
  std::lock_guard lock(mutex);
  info.state = state;
  if (epoch) {
    info.epoch = epoch;
    feedback.epoch = epoch;
  }
  if (!sid.empty()) {
    info.server_sid = std::move(sid);
  }
  if (state != TrackState::ready) {
    frames.clear();
    send = {};
    sender_feedback = {};
    mute_sender = {};
    last_timestamp.reset();
    needs_keyframe = true;
  }
  changed.notify_all();
}

TrackSnapshot TrackData::snapshot() const {
  std::lock_guard lock(mutex);
  auto value = info;
  value.id = config.id;
  value.queued_frames = frames.size();
  value.queued_bytes = 0;
  for (const auto& frame : frames) {
    value.queued_bytes += frame.buffer.size();
  }
  value.outstanding_bytes = budget ? budget->used.load() : 0;
  value.outstanding_frames = budget ? budget->frames.load() : 0;
  return value;
}

WriteStatus TrackData::write(EncodedFrame frame) {
  std::lock_guard lock(mutex);
  if (info.state == TrackState::closed || info.state == TrackState::failed) {
    return WriteStatus::closed;
  }
  if (info.state != TrackState::ready || info.muted || !send) {
    return WriteStatus::not_ready;
  }
  if (auto owner = engine.lock(); !owner || !owner->available()) {
    return WriteStatus::not_ready;
  }
  if (frame.epoch != info.epoch) {
    return WriteStatus::stale_epoch;
  }
  if (!frame.buffer.size() || frame.buffer.size() > config.queue.max_bytes ||
      frame.config_revision != 1 || frame.encoding_id != config.encodings.front().id ||
      frame.duration.count() <= 0 || (last_timestamp && frame.timestamp <= *last_timestamp)) {
    return WriteStatus::invalid_frame;
  }
  if (config.kind == MediaKind::video && needs_keyframe && !frame.keyframe) {
    return WriteStatus::keyframe_required;
  }
  if (!budget || !budget->charge(frame.buffer)) {
    return WriteStatus::backpressure;
  }
  auto status = send(frame);
  if (status == WriteStatus::accepted) {
    ++info.accepted;
    last_timestamp = frame.timestamp;
    needs_keyframe = false;
  }
  return status;
}

void TrackData::receive(EncodedFrame frame) {
  std::lock_guard lock(mutex);
  if (info.state != TrackState::ready) {
    return;
  }
  auto now = SteadyClock::now();
  while (!frames.empty() &&
         now - QueueBudget::admitted(frames.front().buffer) > config.queue.max_age) {
    frames.pop_front();
    ++info.dropped;
    needs_keyframe = true;
  }
  if (config.kind == MediaKind::video && needs_keyframe && !frame.keyframe) {
    ++info.dropped;
    return;
  }
  if (!budget || !budget->charge(frame.buffer)) {
    frames.clear();
    ++info.dropped;
    needs_keyframe = true;
    return;
  }
  frame.epoch = info.epoch;
  frames.push_back(std::move(frame));
  needs_keyframe = false;
  ++info.accepted;
  changed.notify_one();
}

ReadResult TrackData::read(WaitOptions options) {
  if (sdk_thread || options.timeout.count() < 0) {
    return {ReadStatus::failed,
            {},
            make_failure(ErrorDomain::lifecycle,
                         sdk_thread ? ErrorCode::invalid_thread : ErrorCode::invalid_argument,
                         "Invalid blocking read")};
  }
  std::unique_lock exclusive(reader, std::try_to_lock);
  if (!exclusive.owns_lock()) {
    return {ReadStatus::failed,
            {},
            make_failure(ErrorDomain::media, ErrorCode::busy, "Only one reader per track")};
  }
  const auto deadline = SteadyClock::now() + options.timeout;
  std::unique_lock lock(mutex);
  for (;;) {
    if (options.stop.stop_requested()) {
      return {ReadStatus::cancelled, {}, {}};
    }
    if (info.state == TrackState::closed) {
      return {ReadStatus::closed, {}, {}};
    }
    if (info.state == TrackState::failed) {
      return {ReadStatus::failed, {}, info.failure};
    }
    if (info.state != TrackState::ready) {
      return {ReadStatus::suspended, {}, {}};
    }
    while (!frames.empty()) {
      auto frame = std::move(frames.front());
      frames.pop_front();
      if (SteadyClock::now() - QueueBudget::admitted(frame.buffer) > config.queue.max_age) {
        ++info.dropped;
        needs_keyframe = true;
        if (config.kind == MediaKind::video) {
          frames.clear();
        }
        continue;
      }
      ++info.consumed;
      QueueBudget::release_to_caller(frame.buffer);
      return {ReadStatus::frame, std::move(frame), {}};
    }
    if (SteadyClock::now() >= deadline) {
      return {ReadStatus::timeout, {}, {}};
    }
    changed.wait_until(lock,
                       std::min(deadline, SteadyClock::now() + std::chrono::milliseconds(10)));
  }
}
}  // namespace livekit::sdk::client::detail

namespace livekit::sdk::client {
Result<LocalTrack> LocalTrack::create(std::string name, CodecFormat format, QueueLimits limits,
                                      MediaKind kind) {
  static std::atomic<uint64_t> serial{0};
  auto data = std::make_shared<detail::TrackData>();
  data->config.id = "client-track-" + std::to_string(++serial);
  data->config.name = std::move(name);
  data->config.format = std::move(format);
  data->config.kind = kind;
  data->config.queue = limits;
  if (auto result = detail::validate_track(data->config); !result) {
    return Result<LocalTrack>::failure(result.failure_record());
  }
  return Result<LocalTrack>::success(LocalTrack(data));
}

Result<LocalTrack> LocalTrack::create_audio(std::string name, CodecFormat format,
                                            QueueLimits limits) {
  return create(std::move(name), std::move(format), limits, MediaKind::audio);
}

Result<LocalTrack> LocalTrack::create_video(std::string name, CodecFormat format,
                                            QueueLimits limits) {
  return create(std::move(name), std::move(format), limits, MediaKind::video);
}

TrackSnapshot LocalTrack::snapshot() const {
  if (data_) {
    return data_->snapshot();
  }
  TrackSnapshot value;
  value.state = TrackState::closed;
  return value;
}

TrackSnapshot RemoteTrack::snapshot() const {
  if (data_) {
    return data_->snapshot();
  }
  TrackSnapshot value;
  value.state = TrackState::closed;
  return value;
}

SenderFeedback LocalTrack::feedback() const {
  if (!data_) {
    return {};
  }
  std::lock_guard lock(data_->mutex);
  return data_->sender_feedback ? data_->sender_feedback() : data_->feedback;
}

WriteStatus LocalTrack::try_write(EncodedFrame frame) {
  return data_ ? data_->write(std::move(frame)) : WriteStatus::closed;
}

void LocalTrack::set_muted(bool muted) {
  std::shared_ptr<detail::RtcEngine> engine;
  if (data_) {
    std::lock_guard lock(data_->mutex);
    if (!data_->claimed) {
      data_->config.muted = muted;
      data_->info.muted = muted;
      return;
    }
    engine = data_->engine.lock();
  }
  if (!engine) {
    LOG_ERROR("LiveKitClient", "set_muted: detached track");
    return;
  }
  auto result =
      engine->perform([data = data_, muted](detail::RtcSession& s, const detail::Operation&) {
        auto sid = data->snapshot().server_sid;
        if (auto result = s.mute(sid, muted); !result) {
          return result;
        }
        std::lock_guard lock(data->mutex);
        data->info.muted = muted;
        data->config.muted = muted;
        data->needs_keyframe = true;
        ++data->feedback.media_generation;
        if (data->mute_sender) {
          data->mute_sender(muted);
        }
        return Result<void>::success();
      });
  if (!result) {
    LOG_ERROR("LiveKitClient", "set_muted: " << result.error().message);
  }
}

ReadResult RemoteTrack::read(WaitOptions options) {
  return data_ ? data_->read(options) : ReadResult{ReadStatus::closed, {}, {}};
}

void RemoteTrack::request_keyframe() {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    LOG_ERROR("LiveKitClient", "request_keyframe: detached track");
    return;
  }
  auto result = engine->perform([data = data_](detail::RtcSession& s, const detail::Operation&) {
    return s.keyframe(data->snapshot().server_sid);
  });
  if (!result) {
    LOG_ERROR("LiveKitClient", "request_keyframe: " << result.error().message);
  }
}
}  // namespace livekit::sdk::client
