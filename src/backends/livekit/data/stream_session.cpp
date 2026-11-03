#include "livekit/compat/thread.hpp"
#include "livekit/compat/span.hpp"
#include "backends/livekit/data/service_result.hpp"
#include "backends/livekit/data/stream_session.hpp"
#include "livekit/result.hpp"
#include "backends/livekit/rtc_engine.hpp"
#include "support/logger.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <openssl/rand.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace livekit::sdk::client::detail {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t chunk_size = 12000, stream_limit = 256 * 1024, session_limit = 2 * 1024 * 1024,
                 reader_limit = 1024 * 1024;

Failure error(ErrorCode code, const char* text) {
  return make_failure(ErrorDomain::data, code, text);
}

livekit::compat::span<const uint8_t> bytes_of(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

Result<std::string> new_id() {
  std::array<unsigned char, 16> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    return Result<std::string>::failure(
        error(ErrorCode::processing_failed, "Stream ID generation failed"));
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string id;
  for (const auto byte : random) {
    id += hex[byte >> 4];
    id += hex[byte & 15];
  }
  return Result<std::string>::success(std::move(id));
}

Result<Clock::time_point> deadline(const OperationOptions& o) {
  using Return = Result<Clock::time_point>;
  if (o.wait_for_ack || o.timeout.count() < 0 || o.timeout > std::chrono::minutes(5)) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "Invalid stream operation options"));
  }
  return Return::success(Clock::now() + (o.timeout.count() ? o.timeout : std::chrono::seconds(30)));
}

Result<void> valid_string(const std::string& s, size_t max) {
  using Return = Result<void>;
  Utf8State utf8;
  if (s.size() > max || !utf8.append(bytes_of(s)) || utf8.remaining) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "Invalid stream text or metadata"));
  }
  return Return::success();
}
}  // namespace

bool Utf8State::append(livekit::compat::span<const uint8_t> bytes) {
  for (auto c : bytes) {
    if (!remaining) {
      if (c < 0x80) {
        continue;
      }
      if (c >= 0xc2 && c <= 0xdf) {
        remaining = 1;
        value = c & 31;
        minimum = 0x80;
      } else if (c >= 0xe0 && c <= 0xef) {
        remaining = 2;
        value = c & 15;
        minimum = 0x800;
      } else if (c >= 0xf0 && c <= 0xf4) {
        remaining = 3;
        value = c & 7;
        minimum = 0x10000;
      } else {
        return false;
      }
    } else {
      if ((c & 0xc0) != 0x80) {
        return false;
      }
      value = (value << 6) | (c & 63);
      if (!--remaining &&
          (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))) {
        return false;
      }
    }
  }
  return true;
}

void StreamReaderState::finish(Failure failure) {
  std::lock_guard lock(mutex);
  if (closed) {
    return;
  }
  closed = true;
  error = failure;
  if (failure) {
    chunks.clear();
    bytes = 0;
  }
  ready.notify_all();
}

StreamSession::~StreamSession() {
  shutdown();
}

void StreamSession::shutdown() {
  disconnect();
  send_executor_.stop();
  handlers_executor_.stop();
}

void StreamSession::connect() {
  std::lock_guard lock(mutex_);
  if (!connected_) {
    connected_ = true;
    ++generation_;
  }
}

void StreamSession::fail_outgoing(const std::string& id, Failure failure) {
  auto it = writers_.find(id);
  if (it == writers_.end()) {
    return;
  }
  for (auto& b : it->second.batches) {
    if (b->promise) {
      b->promise->set_failure(failure);
    }
  }
  queued_ -= it->second.queued;
  writers_.erase(it);
}

void StreamSession::disconnect() {
  std::lock_guard lock(mutex_);
  connected_ = false;
  ++generation_;
  auto failure = error(ErrorCode::session_closed, "Data stream interrupted by connection change");
  while (!writers_.empty()) {
    fail_outgoing(writers_.begin()->first, failure);
  }
  for (auto& [key, r] : readers_) {
    r->finish(failure);
  }
  readers_.clear();
  cursor_.clear();
}

void StreamSession::participant_left(const std::string& identity) {
  std::lock_guard lock(mutex_);
  for (auto it = readers_.begin(); it != readers_.end();) {
    if (it->first.first == identity) {
      it->second->finish(error(ErrorCode::session_closed, "Stream sender left"));
      it = readers_.erase(it);
    } else {
      ++it;
    }
  }
}

Result<void> StreamSession::register_handler(bool text, const std::string& topic,
                                             StreamHandler handler) {
  using Return = Result<void>;
  if (auto valid = valid_string(topic, 1024); !valid) {
    return Return::failure(valid.failure_record());
  }
  if (!handler) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Empty stream handler"));
  }
  std::lock_guard lock(mutex_);
  if (handlers_.size() >= 256) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::resource_exhausted, "Stream handler limit"));
  }
  if (!handlers_.emplace(std::make_pair(text, topic), std::move(handler)).second) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Duplicate stream handler"));
  }
  return Return::success();
}

void StreamSession::unregister_handler(bool text, const std::string& topic) {
  std::lock_guard lock(mutex_);
  handlers_.erase({text, topic});
}

Result<std::shared_ptr<StreamWriter>> StreamSession::open(bool text, StreamOptions o) {
  using Return = Result<std::shared_ptr<StreamWriter>>;
  if (o.stream_id.empty()) {
    auto id = new_id();
    if (!id) {
      return Return::failure(id.failure_record());
    }
    o.stream_id = std::move(*id);
  }
  if (auto valid = valid_string(o.stream_id, 256); !valid) {
    return Return::failure(valid.failure_record());
  }
  if (auto valid = valid_string(o.topic, 1024); !valid) {
    return Return::failure(valid.failure_record());
  }
  if (auto valid = valid_string(o.name, 1024); !valid) {
    return Return::failure(valid.failure_record());
  }
  if (auto valid = valid_string(o.mime_type, 256); !valid) {
    return Return::failure(valid.failure_record());
  }
  if (auto valid = valid_string(o.reply_to_stream_id, 256); !valid) {
    return Return::failure(valid.failure_record());
  }
  if (o.attributes.size() > 64 || o.destination_identities.size() > 64 ||
      o.attached_stream_ids.size() > 64) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Too much stream metadata"));
  }
  for (auto& [k, v] : o.attributes) {
    if (auto valid = valid_string(k, 256); !valid) {
      return Return::failure(valid.failure_record());
    }
    if (auto valid = valid_string(v, 2048); !valid) {
      return Return::failure(valid.failure_record());
    }
  }
  for (auto& s : o.destination_identities) {
    if (auto valid = valid_string(s, 256); !valid) {
      return Return::failure(valid.failure_record());
    }
  }
  for (auto& s : o.attached_stream_ids) {
    if (auto valid = valid_string(s, 256); !valid) {
      return Return::failure(valid.failure_record());
    }
  }
  if (o.operation < 0 || o.operation > 3 || o.version < 0) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "Invalid text stream operation/version"));
  }
  StreamInfo info;
  info.operation = o.operation;
  info.version = o.version;
  info.generated = o.generated;
  info.id = o.stream_id;
  info.topic = o.topic;
  info.mime_type = text ? "text/plain" : o.mime_type;
  info.name = o.name;
  info.attributes = o.attributes;
  info.total_size = o.total_size;
  info.text = text;
  info.reply_to_stream_id = o.reply_to_stream_id;
  info.attached_stream_ids = o.attached_stream_ids;
  info.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  livekit::DataPacket packet;
  packet.set_kind(0);
  for (auto& d : o.destination_identities) {
    packet.add_destination_identities(d);
  }
  auto* h = packet.mutable_stream_header();
  h->set_stream_id(info.id);
  h->set_timestamp(info.timestamp_ms);
  h->set_topic(info.topic);
  h->set_mime_type(info.mime_type);
  if (info.total_size) {
    h->set_total_length(*info.total_size);
  }
  for (auto& [k, v] : info.attributes) {
    (*h->mutable_attributes())[k] = v;
  }
  if (text) {
    auto* t = h->mutable_text_header();
    t->set_operation_type(static_cast<livekit::DataStream::OperationType>(o.operation));
    t->set_version(o.version);
    t->set_generated(o.generated);
    t->set_reply_to_stream_id(o.reply_to_stream_id);
    for (auto& id : o.attached_stream_ids) {
      t->add_attached_stream_ids(id);
    }
  } else {
    h->mutable_byte_header()->set_name(info.name);
  }
  const auto size = packet.ByteSizeLong();
  if (size > 14000) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                        "Stream header exceeds packet limit"));
  }
  std::lock_guard lock(mutex_);
  if (!connected_) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::session_closed, "Room is disconnected"));
  }
  if (writers_.size() >= 32 || queued_ + size > session_limit) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::resource_exhausted, "Outgoing stream limit"));
  }
  if (writers_.count(info.id)) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Duplicate stream ID"));
  }
  Outgoing out;
  out.info = info;
  out.destinations = std::move(o.destination_identities);
  out.queued = size;
  auto batch = std::make_shared<Batch>();
  batch->deadline = Clock::now() + std::chrono::seconds(30);
  batch->packets.push_back(std::move(packet));
  out.batches.push_back(batch);
  writers_.emplace(info.id, std::move(out));
  queued_ += size;
  return Return::success(std::shared_ptr<StreamWriter>(
      new StreamWriter(weak_from_this(), generation_, std::move(info))));
}

std::future<Result<void>> StreamSession::enqueue(uint64_t generation, const std::string& id,
                                                 livekit::compat::span<const uint8_t> data,
                                                 std::optional<std::string> reason,
                                                 OperationOptions operation) {
  auto promise = std::make_shared<livekit::sdk::client::detail::ServicePromise<void>>();
  auto future = promise->get_future();
  auto submitted = [&]() -> Result<void> {
    const auto end = deadline(operation);
    if (!end) {
      return Result<void>::failure(end.failure_record());
    }
    if (operation.stop.stop_requested()) {
      return Result<void>::failure(error(ErrorCode::cancelled, "Stream write cancelled"));
    }
    if (reason) {
      if (auto valid = valid_string(*reason, 1024); !valid) {
        return valid;
      }
    }
    if (data.size() > stream_limit) {
      return Result<void>::failure(
          error(ErrorCode::resource_exhausted,
                "Stream write exceeds bounded queue; submit smaller blocks"));
    }
    std::lock_guard lock(mutex_);
    auto it = writers_.find(id);
    if (!connected_ || generation != generation_ || it == writers_.end()) {
      return Result<void>::failure(error(ErrorCode::stale_handle, "Stream is closed or stale"));
    }
    auto& out = it->second;
    if (out.closing) {
      return Result<void>::failure(error(ErrorCode::invalid_state, "Stream already closing"));
    }
    if (data.size() > std::numeric_limits<uint64_t>::max() - out.size) {
      return Result<void>::failure(error(ErrorCode::invalid_argument, "Stream length overflow"));
    }
    const auto total = out.size + data.size();
    auto utf8 = out.utf8;
    if (out.info.text && (!utf8.append(data) || (reason && reason->empty() && utf8.remaining))) {
      return Result<void>::failure(
          error(ErrorCode::invalid_argument, "Invalid UTF-8 stream content"));
    }
    if (out.info.total_size && (total > *out.info.total_size ||
                                (reason && reason->empty() && total != *out.info.total_size))) {
      return Result<void>::failure(
          error(ErrorCode::invalid_argument, "Stream total length mismatch"));
    }
    auto batch = std::make_shared<Batch>();
    batch->promise = promise;
    batch->options = operation;
    batch->deadline = *end;
    size_t size = 0;
    uint64_t index = out.index;
    for (size_t offset = 0; offset < data.size();) {
      size_t n = std::min(chunk_size, data.size() - offset);
      livekit::DataPacket p;
      p.set_kind(0);
      for (auto& d : out.destinations) {
        p.add_destination_identities(d);
      }
      auto* c = p.mutable_stream_chunk();
      c->set_stream_id(id);
      c->set_chunk_index(index++);
      c->set_content(data.data() + offset, n);
      if (p.ByteSizeLong() > 15000) {
        return Result<void>::failure(
            error(ErrorCode::invalid_argument, "Stream routing metadata exceeds packet limit"));
      }
      size += p.ByteSizeLong();
      batch->packets.push_back(std::move(p));
      offset += n;
    }
    if (reason) {
      livekit::DataPacket p;
      p.set_kind(0);
      for (auto& d : out.destinations) {
        p.add_destination_identities(d);
      }
      auto* t = p.mutable_stream_trailer();
      t->set_stream_id(id);
      t->set_reason(*reason);
      size += p.ByteSizeLong();
      batch->packets.push_back(std::move(p));
    }
    if (out.queued + size > stream_limit || queued_ + size > session_limit) {
      return Result<void>::failure(error(ErrorCode::resource_exhausted, "Stream queue is full"));
    }
    if (batch->packets.empty()) {
      promise->set_value();
      return Result<void>::success();
    }
    out.index = index;
    out.size = total;
    out.utf8 = utf8;
    out.closing = reason.has_value();
    out.queued += size;
    queued_ += size;
    out.touched = Clock::now();
    out.batches.push_back(batch);
    return Result<void>::success();
  }();
  if (!submitted) {
    promise->set_failure(submitted.failure_record());
  }
  return future;
}

void StreamSession::expire() {
  const auto now = Clock::now();
  for (auto it = writers_.begin(); it != writers_.end();) {
    auto current = it++;
    for (auto& b : current->second.batches) {
      if (b->options.stop.stop_requested() || now >= b->deadline) {
        fail_outgoing(current->first,
                      error(b->sent ? ErrorCode::outcome_unknown
                                    : (b->options.stop.stop_requested() ? ErrorCode::cancelled
                                                                        : ErrorCode::timeout),
                            "Stream operation interrupted; stream is no longer writable"));
        current = writers_.end();
        break;
      }
    }
    if (current != writers_.end() && now - current->second.touched > std::chrono::seconds(60)) {
      fail_outgoing(current->first, error(ErrorCode::timeout, "Outgoing stream idle timeout"));
    }
  }
  for (auto it = readers_.begin(); it != readers_.end();) {
    auto r = it->second;
    bool expired, closed;
    {
      std::lock_guard lock(r->mutex);
      expired = now - r->touched > std::chrono::seconds(60);
      closed = r->closed;
    }
    if (expired || closed) {
      if (expired) {
        r->finish(error(ErrorCode::timeout, "Incoming stream idle timeout"));
      }
      it = readers_.erase(it);
    } else {
      ++it;
    }
  }
}

bool StreamSession::has_outgoing() {
  std::lock_guard lock(mutex_);
  expire();
  return connected_ && queued_ != 0;
}

void StreamSession::pump(const std::function<Result<bool>(const livekit::DataPacket&)>& send,
                         size_t max_packets, const std::function<bool()>& ready) {
  {
    std::lock_guard lock(mutex_);
    expire();
  }
  for (size_t n = 0; n < max_packets; ++n) {
    if (ready && !ready()) {
      return;
    }
    std::string id;
    livekit::DataPacket packet;
    std::shared_ptr<Batch> batch;
    uint64_t generation;
    {
      std::lock_guard lock(mutex_);
      expire();
      if (!connected_ || !queued_) {
        return;
      }
      auto it = writers_.upper_bound(cursor_);
      for (size_t tries = 0; tries < writers_.size(); ++tries) {
        if (it == writers_.end()) {
          it = writers_.begin();
        }
        if (!it->second.batches.empty()) {
          break;
        }
        ++it;
      }
      if (it == writers_.end() || it->second.batches.empty()) {
        return;
      }
      id = it->first;
      cursor_ = id;
      batch = it->second.batches.front();
      packet = batch->packets.front();
      batch->sent = true;
      generation = generation_;
    }
    const auto ok = send(packet);
    std::lock_guard lock(mutex_);
    auto it = writers_.find(id);
    if (generation != generation_ || it == writers_.end()) {
      continue;
    }
    if (!ok || !*ok) {
      fail_outgoing(id,
                    !ok ? ok.failure_record()
                        : error(ErrorCode::outcome_unknown, "Stream channel submission failed"));
      continue;
    }
    auto& out = it->second;
    const auto size = batch->packets.front().ByteSizeLong();
    out.queued -= size;
    queued_ -= size;
    batch->packets.pop_front();
    out.touched = Clock::now();
    if (batch->packets.empty()) {
      if (batch->promise) {
        batch->promise->set_value();
      }
      out.batches.pop_front();
    }
    if (out.closing && out.batches.empty()) {
      writers_.erase(it);
    }
  }
}

void StreamSession::receive(const livekit::DataPacket& packet) {
  StreamHandler handler;
  std::shared_ptr<StreamReaderState> reader;
  const auto& sender = packet.participant_identity();
  if (sender.empty() || packet.ByteSizeLong() > 16384) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (!connected_) {
      return;
    }
    expire();
    if (packet.has_stream_header()) {
      const auto& h = packet.stream_header();
      const auto key = std::make_pair(sender, h.stream_id());
      if (h.stream_id().empty() || h.stream_id().size() > 256 || readers_.count(key) ||
          readers_.size() >= 32 || h.compression() < livekit::DataStream::NONE ||
          h.compression() > livekit::DataStream::DEFLATE_RAW) {
        return;
      }
      const bool text = h.has_text_header();
      if (!text && !h.has_byte_header()) {
        return;
      }
      if (text && (h.text_header().operation_type() < 0 || h.text_header().operation_type() > 3 ||
                   h.text_header().version() < 0)) {
        return;
      }
      auto found = handlers_.find({text, h.topic()});
      if (found == handlers_.end()) {
        return;
      }
      reader = std::make_shared<StreamReaderState>();
      if (h.compression() == livekit::DataStream::DEFLATE_RAW) {
        reader->inflater = std::make_unique<InflateRaw>();
      }
      reader->info.operation = h.text_header().operation_type();
      reader->info.version = h.text_header().version();
      reader->info.generated = h.text_header().generated();
      auto& i = reader->info;
      i.id = h.stream_id();
      i.sender_identity = sender;
      i.topic = h.topic();
      i.mime_type = h.mime_type();
      i.timestamp_ms = h.timestamp();
      i.text = text;
      i.attributes = {h.attributes().begin(), h.attributes().end()};
      if (h.has_total_length()) {
        i.total_size = h.total_length();
      }
      if (text) {
        i.reply_to_stream_id = h.text_header().reply_to_stream_id();
        i.attached_stream_ids = {h.text_header().attached_stream_ids().begin(),
                                 h.text_header().attached_stream_ids().end()};
      } else {
        i.name = h.byte_header().name();
      }
      if (h.has_inline_content()) {
        std::string bytes;
        if (reader->inflater) {
          auto inflated =
              reader->inflater->append(bytes_of(h.inline_content()), reader_limit, true);
          if (!inflated) {
            return;
          }
          bytes = std::move(*inflated);
        } else {
          bytes = h.inline_content();
        }
        if ((i.total_size && *i.total_size != bytes.size()) ||
            (text && (!reader->utf8.append(bytes_of(bytes)) || reader->utf8.remaining))) {
          return;
        }
        reader->chunks.emplace_back(bytes.begin(), bytes.end());
        reader->bytes = bytes.size();
        reader->received = bytes.size();
        reader->closed = true;
      } else {
        readers_.emplace(key, reader);
      }
      handler = found->second;
    } else {
      const auto id = packet.has_stream_chunk()     ? packet.stream_chunk().stream_id()
                      : packet.has_stream_trailer() ? packet.stream_trailer().stream_id()
                                                    : std::string{};
      auto it = readers_.find({sender, id});
      if (it == readers_.end()) {
        return;
      }
      reader = it->second;
      std::lock_guard rlock(reader->mutex);
      if (reader->closed) {
        return;
      }
      auto fail = [&](ErrorCode code, const char* message) {
        reader->closed = true;
        reader->error = error(code, message);
        reader->chunks.clear();
        reader->bytes = 0;
      };
      if (packet.has_stream_chunk()) {
        const auto& c = packet.stream_chunk();
        std::string content;
        auto inflated = reader->inflater
                            ? reader->inflater->append(bytes_of(c.content()), reader_limit)
                            : Result<std::string>::success(c.content());
        if (!inflated) {
          fail(ErrorCode::malformed_message, "Invalid compressed stream chunk");
          reader->ready.notify_all();
          readers_.erase(it);
          return;
        }
        content = std::move(*inflated);
        const auto size = content.size();
        if (c.chunk_index() != reader->index || c.version() != 0 || size > reader_limit ||
            size > std::numeric_limits<uint64_t>::max() - reader->received) {
          fail(ErrorCode::malformed_message, "Invalid stream chunk sequence");
        } else if (reader->bytes + size > reader_limit) {
          fail(ErrorCode::resource_exhausted, "Stream reader is too slow");
        } else if (reader->info.total_size && reader->received + size > *reader->info.total_size) {
          fail(ErrorCode::malformed_message, "Stream exceeds declared size");
        } else if (reader->info.text && !reader->utf8.append(bytes_of(content))) {
          fail(ErrorCode::malformed_message, "Invalid stream UTF-8");
        } else {
          ++reader->index;
          reader->received += size;
          reader->bytes += size;
          reader->chunks.emplace_back(content.begin(), content.end());
        }
      } else {
        const auto& t = packet.stream_trailer();
        if (reader->inflater && t.reason().empty()) {
          if (auto finished = reader->inflater->append({}, reader_limit, true); !finished) {
            fail(ErrorCode::malformed_message, "Truncated compressed stream");
            reader->ready.notify_all();
            readers_.erase(it);
            return;
          }
        }
        if (!t.reason().empty()) {
          fail(ErrorCode::request_failed, "Remote stream ended abnormally");
        } else if ((reader->info.total_size && reader->received != *reader->info.total_size) ||
                   (reader->info.text && reader->utf8.remaining)) {
          fail(ErrorCode::malformed_message, "Truncated stream");
        } else {
          reader->closed = true;
          for (auto& [k, v] : t.attributes()) {
            reader->info.attributes[k] = v;
          }
        }
      }
      reader->touched = Clock::now();
      reader->ready.notify_all();
      if (reader->closed) {
        readers_.erase(it);
      }
      return;
    }
  }
  auto public_reader = std::shared_ptr<StreamReader>(new StreamReader(reader));
  if (!handlers_executor_.post([handler = std::move(handler), public_reader] {
        service_handler_thread = true;
        handler(public_reader);
        service_handler_thread = false;
      })) {
    reader->finish(error(ErrorCode::resource_exhausted, "Stream handler queue is full"));
  }
}

std::future<Result<StreamInfo>> StreamSession::send_text(std::string text, StreamOptions options,
                                                         OperationOptions operation) {
  if (auto valid = valid_string(text, 16 * 1024 * 1024); !valid) {
    return failed_service<StreamInfo>(valid.failure_record());
  }
  auto end = deadline(operation);
  if (!end) {
    return failed_service<StreamInfo>(end.failure_record());
  }
  options.total_size = text.size();
  auto opened = open(true, std::move(options));
  if (!opened) {
    return failed_service<StreamInfo>(opened.failure_record());
  }
  auto writer = *opened;
  auto promise = std::make_shared<ServicePromise<StreamInfo>>();
  auto result = promise->get_future();
  if (!send_executor_.post([writer, text = std::move(text), operation, promise, end = *end] {
        auto fail = [&](Failure failure) {
          (void)writer->finish("error", {});
          promise->set_failure(std::move(failure));
        };
        for (size_t at = 0; at < text.size(); at += chunk_size) {
          if (Clock::now() >= end) {
            fail(error(ErrorCode::timeout, "Stream operation deadline exceeded"));
            return;
          }
          auto next = operation;
          next.timeout = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now());
          auto sent = writer
                          ->submit({reinterpret_cast<const uint8_t*>(text.data() + at),
                                    std::min(chunk_size, text.size() - at)},
                                   next)
                          .get();
          if (!sent) {
            fail(sent.failure_record());
            return;
          }
        }
        if (Clock::now() >= end) {
          fail(error(ErrorCode::timeout, "Stream operation deadline exceeded"));
          return;
        }
        auto next = operation;
        next.timeout = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now());
        auto closed = writer->finish({}, next).get();
        if (!closed) {
          fail(closed.failure_record());
          return;
        }
        promise->set_value(writer->info());
      })) {
    (void)writer->finish("error", {});
    promise->set_failure(error(ErrorCode::resource_exhausted, "Stream sender queue is full"));
  }
  return result;
}

std::future<Result<StreamInfo>> StreamSession::send_file(std::filesystem::path path,
                                                         StreamOptions options,
                                                         OperationOptions operation) {
  auto end = deadline(operation);
  if (!end) {
    return failed_service<StreamInfo>(end.failure_record());
  }
  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!*file) {
    return failed_service<StreamInfo>(
        error(ErrorCode::processing_failed, "Cannot open stream file"));
  }
  file->seekg(0, std::ios::end);
  auto size = file->tellg();
  if (size < 0) {
    return failed_service<StreamInfo>(
        error(ErrorCode::processing_failed, "Cannot determine file length"));
  }
  file->seekg(0);
  options.total_size = static_cast<uint64_t>(size);
  if (options.name.empty()) {
#ifdef _WIN32
    const auto native = path.filename().native();
    if (native.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
      return failed_service<StreamInfo>(
          error(ErrorCode::invalid_argument, "Stream filename is too long"));
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
                            static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
      return failed_service<StreamInfo>(
          error(ErrorCode::invalid_argument, "Stream filename is not valid Unicode"));
    }
    options.name.resize(length);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
                        static_cast<int>(native.size()), options.name.data(), length, nullptr,
                        nullptr);
#else
    options.name = path.filename().native();
#endif
  }
  auto opened = open(false, std::move(options));
  if (!opened) {
    return failed_service<StreamInfo>(opened.failure_record());
  }
  auto writer = *opened;
  auto promise = std::make_shared<ServicePromise<StreamInfo>>();
  auto result = promise->get_future();
  if (!send_executor_.post([writer, file, operation, promise, end = *end] {
        auto fail = [&](Failure failure) {
          (void)writer->finish("error", {});
          promise->set_failure(std::move(failure));
        };
        std::array<uint8_t, chunk_size> buffer;
        while (file->read(reinterpret_cast<char*>(buffer.data()), buffer.size()) ||
               file->gcount()) {
          if (Clock::now() >= end) {
            fail(error(ErrorCode::timeout, "Stream operation deadline exceeded"));
            return;
          }
          auto next = operation;
          next.timeout = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now());
          auto sent =
              writer->submit({buffer.data(), static_cast<size_t>(file->gcount())}, next).get();
          if (!sent) {
            fail(sent.failure_record());
            return;
          }
        }
        if (file->bad()) {
          fail(error(ErrorCode::processing_failed, "File read failed"));
          return;
        }
        if (Clock::now() >= end) {
          fail(error(ErrorCode::timeout, "Stream operation deadline exceeded"));
          return;
        }
        auto next = operation;
        next.timeout = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now());
        auto closed = writer->finish({}, next).get();
        if (!closed) {
          fail(closed.failure_record());
          return;
        }
        promise->set_value(writer->info());
      })) {
    (void)writer->finish("error", {});
    promise->set_failure(error(ErrorCode::resource_exhausted, "Stream sender queue is full"));
  }
  return result;
}

}  // namespace livekit::sdk::client::detail

namespace livekit::sdk::client {
StreamWriter::~StreamWriter() {
  if (auto session = session_.lock()) {
    (void)session->enqueue(generation_, info_.id, {}, std::string("interrupted"), {});
  }
}

StreamInfo StreamReader::info() const {
  std::lock_guard lock(state_->mutex);
  return state_->info;
}

Result<std::optional<std::vector<uint8_t>>> StreamReader::read(std::chrono::milliseconds timeout,
                                                               livekit::compat::stop_token stop) {
  using Return = Result<std::optional<std::vector<uint8_t>>>;
  if (detail::sdk_thread) {
    return Return::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_thread,
                                        "Stream read on SDK callback thread"));
  }
  if (timeout.count() < 0) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Negative read timeout"));
  }
  std::unique_lock lock(state_->mutex);
  const bool ready = state_->ready.wait_for(
      lock, stop, timeout, [&] { return state_->closed || !state_->chunks.empty(); });
  if (stop.stop_requested()) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::cancelled, "Stream read cancelled"));
  }
  if (!ready) {
    return Return::failure(
        make_failure(ErrorDomain::data, ErrorCode::timeout, "Stream read timeout"));
  }
  if (state_->error) {
    return Return::failure(state_->error);
  }
  if (state_->chunks.empty()) {
    return Return::success({});
  }
  auto result = std::move(state_->chunks.front());
  state_->chunks.pop_front();
  state_->bytes -= result.size();
  return Return::success(result);
}

Result<std::vector<uint8_t>> StreamReader::read_all(size_t max, livekit::compat::stop_token stop) {
  std::vector<uint8_t> result;
  for (;;) {
    auto chunk = read(std::chrono::seconds(30), stop);
    if (!chunk) {
      return Result<std::vector<uint8_t>>::failure(chunk.failure_record());
    }
    if (!*chunk) {
      break;
    }
    const auto& bytes = **chunk;
    if (bytes.size() > max - result.size()) {
      close();
      return Result<std::vector<uint8_t>>::failure(make_failure(
          ErrorDomain::data, ErrorCode::resource_exhausted, "Stream read_all limit exceeded"));
    }
    result.insert(result.end(), bytes.begin(), bytes.end());
  }
  return Result<std::vector<uint8_t>>::success(std::move(result));
}

Result<std::string> StreamReader::read_all_text(size_t max, livekit::compat::stop_token stop) {
  if (!info().text) {
    return Result<std::string>::failure(
        make_failure(ErrorDomain::data, ErrorCode::invalid_state, "Not a text stream"));
  }
  auto bytes = read_all(max, stop);
  if (!bytes) {
    return Result<std::string>::failure(bytes.failure_record());
  }
  return Result<std::string>::success(std::string(bytes->begin(), bytes->end()));
}

void StreamReader::close() {
  state_->finish(make_failure(ErrorDomain::data, ErrorCode::cancelled, "Stream reader closed"));
}

std::future<Result<void>> StreamWriter::submit(livekit::compat::span<const uint8_t> bytes,
                                               const OperationOptions& operation) {
  auto session = session_.lock();
  if (!session) {
    return detail::failed_service<void>(
        make_failure(ErrorDomain::data, ErrorCode::session_closed, "Room destroyed"));
  }
  return session->enqueue(generation_, info_.id, bytes, {}, operation);
}

std::future<Result<void>> StreamWriter::finish(const std::string& reason,
                                               const OperationOptions& operation) {
  auto session = session_.lock();
  if (!session) {
    return detail::failed_service<void>(
        make_failure(ErrorDomain::data, ErrorCode::session_closed, "Room destroyed"));
  }
  return session->enqueue(generation_, info_.id, {}, reason, operation);
}

void StreamWriter::write(livekit::compat::span<const uint8_t> bytes, const OperationOptions& op) {
  if (detail::sdk_thread) {
    LOG_ERROR("LiveKitClient", "Stream write on SDK callback thread");
    return;
  }
  auto result = submit(bytes, op).get();
  if (!result) {
    std::lock_guard lock(mutex_);
    failure_ = result.failure_record();
    LOG_ERROR("LiveKitClient", "Stream write failed: " << result.error().message);
  }
}

void StreamWriter::write(const std::string& text, const OperationOptions& op) {
  if (detail::sdk_thread) {
    LOG_ERROR("LiveKitClient", "Stream write on SDK callback thread");
    return;
  }
  auto result = submit({reinterpret_cast<const uint8_t*>(text.data()), text.size()}, op).get();
  if (!result) {
    std::lock_guard lock(mutex_);
    failure_ = result.failure_record();
    LOG_ERROR("LiveKitClient", "Stream text write failed: " << result.error().message);
  }
}

void StreamWriter::close(const std::string& reason, const OperationOptions& op) {
  if (detail::sdk_thread) {
    LOG_ERROR("LiveKitClient", "Stream close on SDK callback thread");
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
  }
  auto result = finish(reason, op).get();
  if (!result) {
    std::lock_guard lock(mutex_);
    failure_ = result.failure_record();
    LOG_ERROR("LiveKitClient", "Stream close failed: " << result.error().message);
  }
}

Result<StreamInfo> StreamWriter::status() const {
  std::lock_guard lock(mutex_);
  return failure_ ? Result<StreamInfo>::failure(failure_) : Result<StreamInfo>::success(info_);
}
}  // namespace livekit::sdk::client
