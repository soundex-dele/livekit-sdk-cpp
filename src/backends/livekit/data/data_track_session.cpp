#include "livekit/compat/thread.hpp"
#include "livekit/compat/span.hpp"
#include "livekit/compat/algorithm.hpp"
#include "backends/livekit/data/service_result.hpp"
#include "backends/livekit/data/data_track_session.hpp"
#include "backends/livekit/data/stream_session.hpp"
#include "backends/livekit/data/packet_encryption.hpp"
#include "livekit/result.hpp"
#include "backends/livekit/rtc_engine.hpp"
#include "support/logger.h"
#include <algorithm>
#include <limits>

namespace livekit::sdk::client::detail {
namespace {
Failure failure(ErrorCode code, const char* message) {
  return make_failure(ErrorDomain::data, code, message);
}

Result<void> text_valid(const std::string& s, size_t max, bool empty = false) {
  using Return = Result<void>;
  Utf8State utf8;
  if ((!empty && s.empty()) || s.size() > max ||
      !utf8.append({reinterpret_cast<const uint8_t*>(s.data()), s.size()}) || utf8.remaining) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "Invalid data track name/encoding"));
  }
  return Return::success();
}

Result<void> encoding_valid(const DataEncoding& e) {
  using Return = Result<void>;
  if (e.custom.empty()) {
    if (e.well_known < 0 || e.well_known > 7) {
      return Return::failure(
          make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Unknown data encoding"));
    }
  } else {
    if (e.well_known) {
      return Return::failure(
          make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Ambiguous data encoding"));
    }
    if (auto valid = text_valid(e.custom, 32); !valid) {
      return Return::failure(valid.failure_record());
    }
  }
  return Return::success();
}

Result<void> validate(const DataTrackOptions& o) {
  using Return = Result<void>;
  if (o.frame_encoding) {
    if (auto valid = encoding_valid(*o.frame_encoding); !valid) {
      return Return::failure(valid.failure_record());
    }
  }
  if (o.schema) {
    if (auto valid = text_valid(o.schema->name, 256); !valid) {
      return Return::failure(valid.failure_record());
    }
    if (auto valid = encoding_valid(o.schema->encoding); !valid) {
      return Return::failure(valid.failure_record());
    }
    if (!o.frame_encoding) {
      return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                          "Schema requires frame encoding"));
    }
  }
  if (!o.frame_encoding || !o.frame_encoding->custom.empty()) {
    return Return::success();
  }
  int frame = o.frame_encoding->well_known, schema = o.schema ? o.schema->encoding.well_known : 0;
  if (frame >= 1 && frame <= 4) {
    if (!o.schema) {
      return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                          "Frame encoding requires schema"));
    }
    if ((frame == 1 && schema != 3) || (frame == 2 && (schema < 4 || schema > 6)) ||
        (frame == 3 && schema != 1) || (frame == 4 && schema != 2)) {
      return Return::failure(
          make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Incompatible data schema"));
    }
  } else if (o.schema && frame == 7 && schema != 7) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "JSON requires JSON schema"));
  }
  return Return::success();
}

template <class T>
void set_encoding(T* wire, const DataEncoding& e) {
  if (!e.custom.empty()) {
    wire->set_custom(e.custom);
  } else {
    wire->set_well_known(static_cast<decltype(wire->well_known())>(e.well_known));
  }
}

template <class T>
DataEncoding parse_encoding(const T& wire) {
  return {static_cast<int>(wire.well_known()), wire.custom()};
}

DataTrackInfo parse(const livekit::DataTrackInfo& wire, const std::string& identity = {}) {
  DataTrackInfo i;
  i.sid = wire.sid();
  i.name = wire.name();
  i.publisher_identity = identity;
  i.encrypted = wire.encryption() != livekit::Encryption::NONE;
  if (wire.has_frame_encoding()) {
    i.options.frame_encoding = parse_encoding(wire.frame_encoding());
  }
  if (wire.has_schema()) {
    i.options.schema = DataSchema{wire.schema().name(), parse_encoding(wire.schema().encoding())};
  }
  return i;
}

Result<std::chrono::steady_clock::time_point> due(const OperationOptions& o) {
  using Return = Result<std::chrono::steady_clock::time_point>;
  if (o.timeout.count() < 0 || o.timeout > std::chrono::minutes(5)) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Invalid DataTrack deadline"));
  }
  return Return::success(std::chrono::steady_clock::now() +
                         (o.timeout.count() ? o.timeout : std::chrono::seconds(10)));
}

uint16_t u16(livekit::compat::span<const uint8_t> p, size_t at) {
  return (uint16_t(p[at]) << 8) | p[at + 1];
}

uint32_t u32(livekit::compat::span<const uint8_t> p, size_t at) {
  return (uint32_t(u16(p, at)) << 16) | u16(p, at + 2);
}

void put16(std::vector<uint8_t>& p, size_t at, uint16_t n) {
  p[at] = uint8_t(n >> 8);
  p[at + 1] = uint8_t(n);
}

void put32(std::vector<uint8_t>& p, size_t at, uint32_t n) {
  put16(p, at, uint16_t(n >> 16));
  put16(p, at + 2, uint16_t(n));
}
}  // namespace

void DataFrameQueue::close(Failure failure) {
  std::lock_guard lock(mutex);
  if (closed) {
    return;
  }
  closed = true;
  error = failure;
  frames.clear();
  bytes = 0;
  ready.notify_all();
}

Result<void> DataTrackSession::configure(EncryptionOptions options) {
  using Return = Result<void>;
  std::lock_guard lock(mutex_);
  if (connected_) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_state,
                                        "Cannot configure connected DataTrack session"));
  }
  encryption_ = std::move(options);
  return Return::success();
}

void DataTrackSession::connect() {
  std::lock_guard lock(mutex_);
  if (!connected_) {
    connected_ = true;
  }
}

void DataTrackSession::clear_remote(Remote& r, Failure error) {
  for (auto& [promise, q, pending] : r.waiters) {
    promise->set_failure(error);
  }
  r.waiters.clear();
  for (auto& weak : r.readers) {
    if (auto q = weak.lock()) {
      q->close(error);
    }
  }
  r.readers.clear();
  r.partials.clear();
  r.subscribed = false;
  r.handle = 0;
}

void DataTrackSession::disconnect() {
  std::lock_guard lock(mutex_);
  connected_ = false;
  ++generation_;
  auto error = failure(ErrorCode::session_closed, "DataTrack session closed");
  for (auto& [h, l] : local_) {
    if (l.publish) {
      l.publish->set_failure(error);
    }
    if (l.unpublish) {
      l.unpublish->set_failure(error);
    }
  }
  for (auto& [id, r] : remote_) {
    clear_remote(r, error);
  }
  local_.clear();
  remote_.clear();
  handles_.clear();
  signals_.clear();
  for (auto& r : restoring_remote_) {
    clear_remote(r, error);
  }
  restoring_remote_.clear();
}

void DataTrackSession::rejoin() {
  std::lock_guard lock(mutex_);
  connected_ = false;
  ++generation_;
  signals_.clear();
  handles_.clear();
  auto error = failure(ErrorCode::session_closed, "DataTrack operation interrupted by rejoin");
  for (auto it = local_.begin(); it != local_.end();) {
    auto& l = it->second;
    if (l.publish || l.unpublish) {
      if (l.publish) {
        l.publish->set_failure(error);
      }
      if (l.unpublish) {
        l.unpublish->set_failure(error);
      }
      it = local_.erase(it);
      continue;
    }
    l.sending.clear();
    l.latest.clear();
    l.restoring = true;
    l.pending = {{}, Clock::now() + std::chrono::seconds(30)};
    l.info.sid.clear();
    livekit::SignalRequest request;
    auto* p = request.mutable_publish_data_track_request();
    p->set_pub_handle(it->first);
    p->set_name(l.info.name);
    p->set_encryption(l.wire.encryption());
    if (l.wire.has_frame_encoding()) {
      *p->mutable_frame_encoding() = l.wire.frame_encoding();
    }
    if (l.wire.has_schema()) {
      *p->mutable_schema() = l.wire.schema();
    }
    signals_.push_back(std::move(request));
    ++it;
  }
  for (auto& [sid, r] : remote_) {
    for (auto& [promise, q, pending] : r.waiters) {
      promise->set_failure(error);
    }
    r.waiters.clear();
    r.partials.clear();
    r.completed.clear();
    r.handle = 0;
    r.subscribed = false;
    for (auto& weak : r.readers) {
      if (auto q = weak.lock()) {
        std::lock_guard qlock(q->mutex);
        q->frames.clear();
        q->bytes = 0;
      }
    }
    if (!r.readers.empty()) {
      restoring_remote_.push_back(std::move(r));
    }
  }
  remote_.clear();
}

std::vector<LocalDataTrack> DataTrackSession::local_tracks() {
  std::lock_guard lock(mutex_);
  std::vector<LocalDataTrack> result;
  for (auto& [h, l] : local_) {
    if (!l.publish && !l.restoring && !l.unpublish) {
      result.push_back(LocalDataTrack(weak_from_this(), generation_, h));
    }
  }
  return result;
}

void DataTrackSession::resume() {
  std::lock_guard lock(mutex_);
  for (auto& [h, l] : local_) {
    l.sending.clear();
    l.latest.clear();
  }
  for (auto& [id, r] : remote_) {
    r.partials.clear();
  }
}

std::future<Result<LocalDataTrack>> DataTrackSession::publish(std::string name,
                                                              DataTrackOptions options,
                                                              OperationOptions operation) {
  auto promise = std::make_shared<livekit::sdk::client::detail::ServicePromise<LocalDataTrack>>();
  auto future = promise->get_future();
  auto submitted = [&]() -> Result<void> {
    if (auto valid = text_valid(name, 256); !valid) {
      return valid;
    }
    if (auto valid = validate(options); !valid) {
      return valid;
    }
    auto end = due(operation);
    if (!end) {
      return Result<void>::failure(end.failure_record());
    }
    if (operation.stop.stop_requested()) {
      return Result<void>::failure(failure(ErrorCode::cancelled, "DataTrack publish cancelled"));
    }
    std::lock_guard lock(mutex_);
    if (auto ready = tick(); !ready) {
      return ready;
    }
    if (!connected_) {
      return Result<void>::failure(failure(ErrorCode::session_closed, "Room disconnected"));
    }
    if (local_.size() >= 32 || next_handle_ >= 65535) {
      return Result<void>::failure(
          failure(ErrorCode::resource_exhausted, "DataTrack publication limit"));
    }
    for (auto& [h, l] : local_) {
      if (l.info.name == name) {
        return Result<void>::failure(
            failure(ErrorCode::invalid_argument, "Duplicate data track name"));
      }
    }
    const auto handle = static_cast<uint16_t>(++next_handle_);
    livekit::SignalRequest request;
    auto* p = request.mutable_publish_data_track_request();
    p->set_pub_handle(handle);
    p->set_name(name);
    if (encryption_.keys && encryption_.data) {
      p->set_encryption(livekit::Encryption::GCM);
    }
    if (options.frame_encoding) {
      set_encoding(p->mutable_frame_encoding(), *options.frame_encoding);
    }
    if (options.schema) {
      p->mutable_schema()->set_name(options.schema->name);
      set_encoding(p->mutable_schema()->mutable_encoding(), options.schema->encoding);
    }
    Local local;
    local.info.name = name;
    local.info.options = std::move(options);
    local.publish = promise;
    local.pending = {operation, *end};
    local.wire.set_pub_handle(handle);
    local_.emplace(handle, std::move(local));
    signals_.push_back(std::move(request));
    return Result<void>::success();
  }();
  if (!submitted) {
    promise->set_failure(submitted.failure_record());
  }
  return future;
}

std::future<Result<void>> DataTrackSession::unpublish(uint64_t generation, uint16_t handle,
                                                      OperationOptions operation) {
  auto promise = std::make_shared<livekit::sdk::client::detail::ServicePromise<void>>();
  auto future = promise->get_future();
  auto submitted = [&]() -> Result<void> {
    auto end = due(operation);
    if (!end) {
      return Result<void>::failure(end.failure_record());
    }
    if (operation.stop.stop_requested()) {
      return Result<void>::failure(failure(ErrorCode::cancelled, "DataTrack unpublish cancelled"));
    }
    std::lock_guard lock(mutex_);
    auto it = local_.find(handle);
    if (!connected_ || generation != generation_ || it == local_.end() || it->second.publish ||
        it->second.restoring) {
      return Result<void>::failure(failure(ErrorCode::stale_handle, "DataTrack is stale"));
    }
    if (it->second.unpublish) {
      return Result<void>::failure(
          failure(ErrorCode::invalid_state, "DataTrack unpublish pending"));
    }
    it->second.unpublish = promise;
    it->second.pending = {operation, *end};
    it->second.sending.clear();
    it->second.latest.clear();
    livekit::SignalRequest request;
    request.mutable_unpublish_data_track_request()->set_pub_handle(handle);
    signals_.push_back(std::move(request));
    return Result<void>::success();
  }();
  if (!submitted) {
    promise->set_failure(submitted.failure_record());
  }
  return future;
}

Result<DataTrackInfo> DataTrackSession::info(uint64_t generation, uint16_t handle) {
  using Return = Result<DataTrackInfo>;
  std::lock_guard lock(mutex_);
  if (generation != generation_ || !local_.count(handle)) {
    return Return::failure(failure(ErrorCode::stale_handle, "DataTrack is stale"));
  }
  return Return::success(local_.find(handle)->second.info);
}

Result<DataTrackStats> DataTrackSession::stats(uint64_t generation, uint16_t handle) {
  using Return = Result<DataTrackStats>;
  std::lock_guard lock(mutex_);
  if (generation != generation_ || !local_.count(handle)) {
    return Return::failure(failure(ErrorCode::stale_handle, "DataTrack is stale"));
  }
  return Return::success(local_.find(handle)->second.stats);
}

Result<void> DataTrackSession::write(uint64_t generation, uint16_t handle, DataFrame frame) {
  using Return = Result<void>;
  if (frame.payload.empty() || frame.payload.size() > 1024 * 1024) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "DataTrack frame must contain 1..1048576 bytes"));
  }
  std::lock_guard lock(mutex_);
  auto it = local_.find(handle);
  if (!connected_ || generation != generation_ || it == local_.end() || it->second.publish ||
      it->second.restoring || it->second.unpublish) {
    return Return::failure(failure(ErrorCode::stale_handle, "DataTrack is not published"));
  }
  auto& l = it->second;
  std::optional<SealedPayload> sealed;
  if (l.info.encrypted) {
    if (!encryption_.keys) {
      return Return::failure(
          make_failure(ErrorDomain::data, ErrorCode::invalid_state, "DataTrack key unavailable"));
    }
    auto encrypted = encrypt_payload(*encryption_.keys, frame.payload);
    if (!encrypted) {
      return Return::failure(encrypted.failure_record());
    }
    sealed = std::move(*encrypted);
    frame.payload = sealed->ciphertext;
  }
  const size_t extensions = (frame.user_timestamp ? 10 : 0) + (sealed ? 15 : 0);
  const size_t header = 12 + (extensions ? ((extensions + 2 + 3) / 4) * 4 : 0),
               capacity = 16000 - header;
  std::deque<std::vector<uint8_t>> packets;
  auto sequence = l.sequence;
  const auto frame_number = l.frame_number++;
  const auto ticks = uint32_t(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
          .count() *
      9 / 100);
  for (size_t at = 0; at < frame.payload.size(); at += capacity) {
    const auto count = std::min(capacity, frame.payload.size() - at);
    std::vector<uint8_t> p(header + count);
    p[0] =
        (at == 0 ? 0x10 : 0) | (at + count == frame.payload.size() ? 8 : 0) | (extensions ? 4 : 0);
    put16(p, 2, handle);
    put16(p, 4, sequence++);
    put16(p, 6, frame_number);
    put32(p, 8, ticks);
    if (extensions) {
      put16(p, 12, uint16_t((header - 12) / 4 - 1));
      size_t ext = 14;
      if (sealed) {
        p[ext++] = 1;
        p[ext++] = 13;
        p[ext++] = sealed->key_index;
        std::copy(sealed->iv.begin(), sealed->iv.end(), p.begin() + ext);
        ext += 12;
      }
      if (frame.user_timestamp) {
        p[ext++] = 2;
        p[ext++] = 8;
        for (size_t i = 0; i < 8; ++i) {
          p[ext + i] = uint8_t(*frame.user_timestamp >> (56 - 8 * i));
        }
      }
    }
    std::copy_n(frame.payload.begin() + at, count, p.begin() + header);
    packets.push_back(std::move(p));
  }
  if (!l.latest.empty()) {
    ++l.stats.dropped;
  }
  l.latest = std::move(packets);
  l.sequence = sequence;
  ++l.stats.accepted;
  return Return::success();
}

std::vector<RemoteDataTrack> DataTrackSession::remote_tracks() {
  std::lock_guard lock(mutex_);
  std::vector<RemoteDataTrack> result;
  for (auto& [sid, r] : remote_) {
    result.push_back(RemoteDataTrack(weak_from_this(), generation_, r.info));
  }
  return result;
}

void DataTrackSession::subscription_signal(const std::string& sid, bool subscribe,
                                           std::optional<uint32_t> fps) {
  livekit::SignalRequest request;
  auto* u = request.mutable_update_data_subscription()->add_updates();
  u->set_track_sid(sid);
  u->set_subscribe(subscribe);
  if (fps) {
    u->mutable_options()->set_target_fps(*fps);
  }
  signals_.push_back(std::move(request));
}

std::future<Result<std::shared_ptr<DataTrackStream>>> DataTrackSession::subscribe(
    uint64_t generation, const std::string& sid, DataSubscribeOptions options,
    OperationOptions operation) {
  auto promise = std::make_shared<
      livekit::sdk::client::detail::ServicePromise<std::shared_ptr<DataTrackStream>>>();
  auto future = promise->get_future();
  auto submitted = [&]() -> Result<void> {
    auto end = due(operation);
    if (!end) {
      return Result<void>::failure(end.failure_record());
    }
    if (operation.stop.stop_requested()) {
      return Result<void>::failure(failure(ErrorCode::cancelled, "DataTrack subscribe cancelled"));
    }
    if (options.buffer_frames < 1 || options.buffer_frames > 64 ||
        (options.target_fps && (!*options.target_fps || *options.target_fps > 1000))) {
      return Result<void>::failure(
          failure(ErrorCode::invalid_argument, "Invalid DataTrack subscription limits"));
    }
    std::lock_guard lock(mutex_);
    auto it = remote_.find(sid);
    if (!connected_ || generation != generation_ || it == remote_.end()) {
      return Result<void>::failure(failure(ErrorCode::stale_handle, "Remote DataTrack is stale"));
    }
    auto& r = it->second;
    if (r.info.encrypted && (!encryption_.keys || !encryption_.data)) {
      return Result<void>::failure(
          failure(ErrorCode::unsupported, "Encrypted DataTrack requires a configured decryptor"));
    }
    if (r.readers.size() + r.waiters.size() >= 16) {
      return Result<void>::failure(
          failure(ErrorCode::resource_exhausted, "DataTrack consumer limit"));
    }
    auto q = std::make_shared<DataFrameQueue>();
    q->capacity = options.buffer_frames;
    if (r.subscribed) {
      r.readers.push_back(q);
      promise->set_value(std::shared_ptr<DataTrackStream>(new DataTrackStream(q)));
    } else {
      if (r.waiters.empty()) {
        r.pending = {operation, *end};
        r.target_fps = options.target_fps;
        subscription_signal(sid, true, options.target_fps);
      }
      r.waiters.push_back({promise, q, {operation, *end}});
    }
    return Result<void>::success();
  }();
  if (!submitted) {
    promise->set_failure(submitted.failure_record());
  }
  return future;
}

void DataTrackSession::participant(const livekit::ParticipantInfo& p) {
  std::lock_guard lock(mutex_);
  if (p.identity().empty()) {
    return;
  }
  for (auto it = remote_.begin(); it != remote_.end();) {
    bool exists = false;
    for (auto& info : p.data_tracks()) {
      if (info.sid() == it->first) {
        exists = true;
      }
    }
    if (it->second.info.publisher_identity == p.identity() && (p.state() == 3 || !exists)) {
      handles_.erase(it->second.handle);
      clear_remote(it->second, failure(ErrorCode::session_closed, "Remote DataTrack unpublished"));
      it = remote_.erase(it);
    } else {
      ++it;
    }
  }
  if (p.state() == 3) {
    return;
  }
  for (auto& info : p.data_tracks()) {
    if (!info.sid().empty() && info.pub_handle() > 0 && info.pub_handle() <= 65535 &&
        remote_.size() < 128) {
      auto& r = remote_[info.sid()];
      r.info = parse(info, p.identity());
      auto saved = std::find_if(restoring_remote_.begin(), restoring_remote_.end(), [&](auto& old) {
        return old.info.publisher_identity == p.identity() && old.info.name == info.name();
      });
      if (saved != restoring_remote_.end()) {
        r.readers = std::move(saved->readers);
        r.target_fps = saved->target_fps;
        restoring_remote_.erase(saved);
        subscription_signal(info.sid(), true, r.target_fps);
      }
    }
  }
}

Result<void> DataTrackSession::receive_signal(const livekit::SignalResponse& response) {
  using Return = Result<void>;
  std::lock_guard lock(mutex_);
  if (auto ready = tick(); !ready) {
    return Return::failure(ready.failure_record());
  }
  if (response.has_publish_data_track_response()) {
    const auto& info = response.publish_data_track_response().info();
    if (info.pub_handle() == 0 || info.pub_handle() > 65535 || info.sid().empty()) {
      return Return::success();
    }
    auto it = local_.find(uint16_t(info.pub_handle()));
    if (it == local_.end()) {
      livekit::SignalRequest r;
      r.mutable_unpublish_data_track_request()->set_pub_handle(info.pub_handle());
      signals_.push_back(std::move(r));
      return Return::success();
    }
    auto& l = it->second;
    if (!l.publish && !l.restoring) {
      return Return::success();
    }
    l.restoring = false;
    l.info = parse(info);
    l.wire = info;
    auto promise = std::move(l.publish);
    if (promise) {
      promise->set_value(
          LocalDataTrack(weak_from_this(), generation_, uint16_t(info.pub_handle())));
    }
  } else if (response.has_unpublish_data_track_response()) {
    const auto handle = response.unpublish_data_track_response().info().pub_handle();
    if (handle > 65535) {
      return Return::success();
    }
    auto it = local_.find(uint16_t(handle));
    if (it == local_.end()) {
      return Return::success();
    }
    if (it->second.unpublish) {
      it->second.unpublish->set_value();
    }
    if (it->second.publish) {
      it->second.publish->set_failure(
          failure(ErrorCode::request_failed, "DataTrack unpublished before confirmation"));
    }
    local_.erase(it);
  } else if (response.has_data_track_subscriber_handles()) {
    for (auto& [handle, track] : response.data_track_subscriber_handles().sub_handles()) {
      if (!handle || handle > 65535) {
        continue;
      }
      auto it = remote_.find(track.track_sid());
      if (it == remote_.end() || it->second.info.publisher_identity != track.publisher_identity()) {
        continue;
      }
      auto& r = it->second;
      if (!r.subscribed && r.waiters.empty() && r.readers.empty()) {
        continue;
      }
      if (r.handle != handle) {
        handles_.erase(r.handle);
        r.partials.clear();
        r.completed.clear();
      }
      r.handle = uint16_t(handle);
      r.subscribed = true;
      handles_[r.handle] = track.track_sid();
      for (auto& [promise, q, pending] : r.waiters) {
        r.readers.push_back(q);
        promise->set_value(std::shared_ptr<DataTrackStream>(new DataTrackStream(q)));
      }
      r.waiters.clear();
    }
  } else if (response.has_request_response()) {
    const auto& r = response.request_response();
    if (r.reason() == livekit::RequestResponse::OK ||
        r.reason() == livekit::RequestResponse::QUEUED) {
      return Return::success();
    }
    const uint32_t handle = r.has_publish_data_track()     ? r.publish_data_track().pub_handle()
                            : r.has_unpublish_data_track() ? r.unpublish_data_track().pub_handle()
                                                           : 0;
    if (!handle || handle > 65535) {
      return Return::success();
    }
    auto it = local_.find(uint16_t(handle));
    if (it == local_.end()) {
      return Return::success();
    }
    auto error = make_failure(ErrorDomain::data,
                              r.reason() == livekit::RequestResponse::NOT_ALLOWED
                                  ? ErrorCode::permission_denied
                                  : ErrorCode::request_failed,
                              "DataTrack request rejected", false, int(r.reason()));
    if (it->second.restoring) {
      return Return::failure(error);
    }
    if (it->second.publish) {
      it->second.publish->set_failure(error);
      local_.erase(it);
    } else if (it->second.unpublish) {
      it->second.unpublish->set_failure(error);
      it->second.unpublish.reset();
    }
  }
  return Return::success();
}

Result<void> DataTrackSession::tick() {
  using Return = Result<void>;
  const auto now = Clock::now();
  for (auto it = local_.begin(); it != local_.end();) {
    auto& l = it->second;
    if (l.restoring && now >= l.pending.deadline) {
      return Return::failure(make_failure(ErrorDomain::data, ErrorCode::recovery_failed,
                                          "DataTrack publication recovery timed out"));
    }
    if ((l.publish || l.unpublish) &&
        (l.pending.operation.stop.stop_requested() || now >= l.pending.deadline)) {
      auto error =
          failure(l.pending.sent ? ErrorCode::outcome_unknown
                                 : (l.pending.operation.stop.stop_requested() ? ErrorCode::cancelled
                                                                              : ErrorCode::timeout),
                  "DataTrack operation interrupted");
      if (l.publish) {
        l.publish->set_failure(error);
      }
      if (l.unpublish) {
        l.unpublish->set_failure(error);
      }
      const auto handle = it->first;
      livekit::compat::erase_if(signals_, [&](const auto& r) {
        return (r.has_publish_data_track_request() &&
                r.publish_data_track_request().pub_handle() == handle) ||
               (r.has_unpublish_data_track_request() &&
                r.unpublish_data_track_request().pub_handle() == handle);
      });
      if (l.pending.sent) {
        livekit::SignalRequest request;
        request.mutable_unpublish_data_track_request()->set_pub_handle(handle);
        signals_.push_back(std::move(request));
      }
      it = local_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [sid, r] : remote_) {
    const bool had_waiters = !r.waiters.empty();
    for (auto it = r.waiters.begin(); it != r.waiters.end();) {
      if (it->pending.operation.stop.stop_requested() || now >= it->pending.deadline) {
        it->promise->set_failure(failure(
            it->pending.operation.stop.stop_requested() ? ErrorCode::cancelled : ErrorCode::timeout,
            "DataTrack subscription interrupted"));
        it = r.waiters.erase(it);
      } else {
        ++it;
      }
    }
    if (had_waiters && r.waiters.empty() && r.readers.empty() && !r.subscribed) {
      subscription_signal(sid, false);
    }
    livekit::compat::erase_if(r.readers, [](auto& weak) {
      auto q = weak.lock();
      if (!q) {
        return true;
      }
      std::lock_guard lock(q->mutex);
      return q->closed;
    });
    if (r.subscribed && r.readers.empty() && r.waiters.empty()) {
      handles_.erase(r.handle);
      r.subscribed = false;
      r.handle = 0;
      r.partials.clear();
      subscription_signal(sid, false);
    }
    for (auto it = r.partials.begin(); it != r.partials.end();) {
      if (now - it->second.created > std::chrono::milliseconds(500)) {
        ++r.stats.expired;
        it = r.partials.erase(it);
      } else {
        ++it;
      }
    }
  }
  return Return::success();
}

Result<bool> DataTrackSession::needs_publisher() {
  using Return = Result<bool>;
  std::lock_guard lock(mutex_);
  if (auto ready = tick(); !ready) {
    return Return::failure(ready.failure_record());
  }
  return Return::success(!local_.empty());
}

Result<std::vector<livekit::SignalRequest>> DataTrackSession::take_signals() {
  using Return = Result<std::vector<livekit::SignalRequest>>;
  std::lock_guard lock(mutex_);
  if (auto ready = tick(); !ready) {
    return Return::failure(ready.failure_record());
  }
  std::vector<livekit::SignalRequest> result;
  while (!signals_.empty()) {
    auto request = std::move(signals_.front());
    signals_.pop_front();
    uint32_t h = request.has_publish_data_track_request()
                     ? request.publish_data_track_request().pub_handle()
                 : request.has_unpublish_data_track_request()
                     ? request.unpublish_data_track_request().pub_handle()
                     : 0;
    if (h && h <= 65535 && local_.count(uint16_t(h))) {
      local_.find(uint16_t(h))->second.pending.sent = true;
    }
    result.push_back(std::move(request));
  }
  return Return::success(result);
}

Result<std::optional<std::vector<uint8_t>>> DataTrackSession::take_packet() {
  using Return = Result<std::optional<std::vector<uint8_t>>>;
  std::lock_guard lock(mutex_);
  if (auto ready = tick(); !ready) {
    return Return::failure(ready.failure_record());
  }
  if (!connected_ || local_.empty()) {
    return Return::success({});
  }
  auto it = local_.upper_bound(send_cursor_);
  for (size_t n = 0; n < local_.size(); ++n) {
    if (it == local_.end()) {
      it = local_.begin();
    }
    auto& l = it->second;
    if (l.sending.empty()) {
      l.sending.swap(l.latest);
    }
    if (!l.sending.empty()) {
      send_cursor_ = it->first;
      auto p = std::move(l.sending.front());
      l.sending.pop_front();
      return Return::success(p);
    }
    ++it;
  }
  return Return::success({});
}

void DataTrackSession::packet_sent(livekit::compat::span<const uint8_t> p, bool success) {
  if (p.size() < 12) {
    return;
  }
  std::lock_guard lock(mutex_);
  auto it = local_.find(u16(p, 2));
  if (it == local_.end()) {
    return;
  }
  if (!success) {
    ++it->second.stats.dropped;
    it->second.sending.clear();
  } else if (p[0] & 8) {
    ++it->second.stats.delivered;
  }
}

void DataTrackSession::sync(livekit::SyncState& state) {
  std::lock_guard lock(mutex_);
  for (auto& [h, l] : local_) {
    if (!l.publish && !l.restoring && !l.unpublish) {
      *state.add_publish_data_tracks()->mutable_info() = l.wire;
    }
  }
}

Result<void> DataTrackSession::receive_packet(livekit::compat::span<const uint8_t> p) {
  using Return = Result<void>;
  if (p.size() < 12 || p.size() > 16384 || (p[0] >> 5) != 0) {
    return Return::success();
  }
  std::lock_guard lock(mutex_);
  if (auto ready = tick(); !ready) {
    return Return::failure(ready.failure_record());
  }
  if (!connected_) {
    return Return::success();
  }
  auto found = handles_.find(u16(p, 2));
  if (found == handles_.end()) {
    return Return::success();
  }
  auto ri = remote_.find(found->second);
  if (ri == remote_.end()) {
    return Return::success();
  }
  auto& r = ri->second;
  size_t offset = 12;
  std::optional<uint64_t> user;
  std::vector<uint8_t> crypt;
  if (p[0] & 4) {
    if (p.size() < 14) {
      ++r.stats.malformed;
      return Return::success();
    }
    offset = 12 + (size_t(u16(p, 12)) + 1) * 4;
    if (offset > p.size()) {
      ++r.stats.malformed;
      return Return::success();
    }
    for (size_t at = 14; at < offset;) {
      const auto id = p[at];
      if (!id) {
        break;
      }
      if (at + 2 > offset || size_t(p[at + 1]) > offset - at - 2) {
        ++r.stats.malformed;
        return Return::success();
      }
      const size_t n = p[at + 1];
      if (id == 1) {
        if (n != 13 || !crypt.empty()) {
          ++r.stats.malformed;
          return Return::success();
        }
        crypt.assign(p.begin() + at + 2, p.begin() + at + 2 + n);
      }
      if (id == 2 && n >= 8) {
        uint64_t v = 0;
        for (size_t j = 0; j < 8; ++j) {
          v = (v << 8) | p[at + 2 + j];
        }
        user = v;
      }
      at += 2 + n;
    }
  }
  const bool start = p[0] & 16, final = p[0] & 8;
  const uint16_t frame = u16(p, 6), sequence = u16(p, 4);
  const uint32_t timestamp = u32(p, 8);
  if (std::find(r.completed.begin(), r.completed.end(), std::make_pair(frame, timestamp)) !=
      r.completed.end()) {
    return Return::success();
  }
  auto it = r.partials.find(frame);
  if (it == r.partials.end()) {
    if (r.partials.size() >= 8) {
      auto oldest = std::min_element(r.partials.begin(), r.partials.end(), [](auto& a, auto& b) {
        return a.second.created < b.second.created;
      });
      r.partials.erase(oldest);
      ++r.stats.dropped;
    }
    Partial part;
    part.timestamp = timestamp;
    part.user_timestamp = user;
    part.encryption = crypt;
    it = r.partials.emplace(frame, std::move(part)).first;
  }
  auto& partial = it->second;
  if (crypt != partial.encryption || timestamp != partial.timestamp ||
      user != partial.user_timestamp ||
      (!partial.chunks.count(sequence) && partial.chunks.size() >= 128) ||
      (!partial.chunks.count(sequence) && partial.bytes + p.size() - offset > 1024 * 1024 + 16)) {
    r.partials.erase(it);
    ++r.stats.malformed;
    return Return::success();
  }
  if (start) {
    if (partial.has_start && partial.start != sequence) {
      r.partials.erase(it);
      ++r.stats.malformed;
      return Return::success();
    }
    partial.start = sequence;
    partial.has_start = true;
  }
  if (final) {
    if (partial.end && *partial.end != sequence) {
      r.partials.erase(it);
      ++r.stats.malformed;
      return Return::success();
    }
    partial.end = sequence;
  }
  if (!partial.chunks.count(sequence)) {
    partial.bytes += p.size() - offset;
    partial.chunks.emplace(sequence, std::vector<uint8_t>(p.begin() + offset, p.end()));
  }
  if (!partial.has_start || !partial.end) {
    return Return::success();
  }
  const auto expected = uint16_t(*partial.end - partial.start) + 1;
  if (expected > 128) {
    r.partials.erase(it);
    ++r.stats.malformed;
    return Return::success();
  }
  if (partial.chunks.size() != expected) {
    return Return::success();
  }
  DataFrame output;
  output.user_timestamp = partial.user_timestamp;
  output.payload.reserve(partial.bytes);
  for (uint16_t i = 0; i < expected; ++i) {
    auto c = partial.chunks.find(uint16_t(partial.start + i));
    if (c == partial.chunks.end()) {
      return Return::success();
    }
    output.payload.insert(output.payload.end(), c->second.begin(), c->second.end());
  }
  auto encryption = partial.encryption;
  r.partials.erase(it);
  {
    if (!encryption.empty()) {
      if (!encryption_.keys || !encryption_.data) {
        ++r.stats.malformed;
        return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                            "No DataTrack decryptor"));
      }
      auto decrypted = decrypt_payload(*encryption_.keys, encryption.front(),
                                       livekit::compat::span<const uint8_t>(encryption).subspan(1),
                                       output.payload);
      if (!decrypted) {
        ++r.stats.malformed;
        return Return::failure(decrypted.failure_record());
      }
      output.payload = std::move(*decrypted);
    } else if (r.info.encrypted ||
               (encryption_.keys && encryption_.data && encryption_.require_encrypted)) {
      ++r.stats.malformed;
      return Return::failure(make_failure(ErrorDomain::data, ErrorCode::permission_denied,
                                          "Unencrypted DataTrack rejected"));
    }
  }
  r.completed.emplace_back(frame, timestamp);
  if (r.completed.size() > 64) {
    r.completed.pop_front();
  }
  ++r.stats.delivered;
  for (auto& weak : r.readers) {
    if (auto q = weak.lock()) {
      std::lock_guard qlock(q->mutex);
      if (q->closed) {
        continue;
      }
      while (!q->frames.empty() && (q->frames.size() >= q->capacity ||
                                    q->bytes + output.payload.size() > 2 * 1024 * 1024)) {
        q->bytes -= q->frames.front().payload.size();
        q->frames.pop_front();
        ++q->stats.dropped;
      }
      q->bytes += output.payload.size();
      q->frames.push_back(output);
      ++q->stats.accepted;
      q->ready.notify_all();
    }
  }
  return Return::success();
}
}  // namespace livekit::sdk::client::detail

namespace livekit::sdk::client {

DataTrackStream::~DataTrackStream() {
  close();
}

void DataTrackStream::close() {
  state_->close();
}

DataTrackStats DataTrackStream::stats() const {
  std::lock_guard lock(state_->mutex);
  return state_->stats;
}

Result<std::optional<DataFrame>> DataTrackStream::read(std::chrono::milliseconds timeout,
                                                       livekit::compat::stop_token stop) {
  using Return = Result<std::optional<DataFrame>>;
  if (detail::sdk_thread) {
    return Return::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_thread,
                                        "Data track read on SDK callback thread"));
  }
  if (timeout.count() < 0) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Negative DataTrack timeout"));
  }
  std::unique_lock lock(state_->mutex);
  const auto ready = state_->ready.wait_for(
      lock, stop, timeout, [&] { return state_->closed || !state_->frames.empty(); });
  if (stop.stop_requested()) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::cancelled, "DataTrack read cancelled"));
  }
  if (!ready) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::timeout, "DataTrack read timeout"));
  }
  if (state_->error) {
    return Return::failure(state_->error);
  }
  if (state_->frames.empty()) {
    return Return::success({});
  }
  auto frame = std::move(state_->frames.front());
  state_->frames.pop_front();
  state_->bytes -= frame.payload.size();
  ++state_->stats.delivered;
  return Return::success(frame);
}

Result<DataTrackInfo> LocalDataTrack::info() const {
  auto session = session_.lock();
  if (!session) {
    return Result<DataTrackInfo>::failure(
        make_failure(ErrorDomain::data, ErrorCode::session_closed, "Room destroyed"));
  }
  return session->info(generation_, handle_);
}

Result<DataTrackStats> LocalDataTrack::stats() const {
  auto session = session_.lock();
  if (!session) {
    return Result<DataTrackStats>::failure(
        make_failure(ErrorDomain::data, ErrorCode::session_closed, "Room destroyed"));
  }
  return session->stats(generation_, handle_);
}

void LocalDataTrack::write(DataFrame frame) const {
  if (detail::sdk_thread) {
    LOG_ERROR("LiveKitClient", "Data track write on SDK callback thread");
    return;
  }
  auto session = session_.lock();
  if (!session) {
    LOG_ERROR("LiveKitClient", "Data track room destroyed");
    return;
  }
  auto result = session->write(generation_, handle_, std::move(frame));
  if (!result) {
    LOG_ERROR("LiveKitClient", "Data track write failed: " << result.error().message);
  }
}

void LocalDataTrack::unpublish(OperationOptions operation) const {
  if (detail::sdk_thread) {
    LOG_ERROR("LiveKitClient", "Data track unpublish on SDK callback thread");
    return;
  }
  auto session = session_.lock();
  if (!session) {
    LOG_ERROR("LiveKitClient", "Data track room destroyed");
    return;
  }
  auto result = session->unpublish(generation_, handle_, operation).get();
  if (!result) {
    LOG_ERROR("LiveKitClient", "Data track unpublish failed: " << result.error().message);
  }
}

Result<std::shared_ptr<DataTrackStream>> RemoteDataTrack::subscribe(
    DataSubscribeOptions options, OperationOptions operation) const {
  using Return = Result<std::shared_ptr<DataTrackStream>>;
  if (detail::sdk_thread) {
    return Return::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_thread,
                                        "Data track subscribe on SDK callback thread"));
  }
  auto session = session_.lock();
  if (!session) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::session_closed, "Room destroyed"));
  }
  return session->subscribe(generation_, info_.sid, options, operation).get();
}
}  // namespace livekit::sdk::client
