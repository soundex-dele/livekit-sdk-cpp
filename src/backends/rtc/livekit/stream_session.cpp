#include "backends/rtc/livekit/stream_session.hpp"
#include "livekit/error.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <random>

namespace livekit::sdk::detail {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t chunk_size = 12000, stream_limit = 256 * 1024, session_limit = 2 * 1024 * 1024,
                 reader_limit = 1024 * 1024;

std::exception_ptr error(ErrorCode code, const char* text) {
  return std::make_exception_ptr(SDKError(ErrorDomain::data, code, text));
}

std::span<const uint8_t> bytes_of(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string new_id() {
  std::random_device rng;
  static constexpr char hex[] = "0123456789abcdef";
  std::string s(32, '0');
  for (size_t i = 0; i < s.size(); i += 2) {
    auto v = rng();
    s[i] = hex[v & 15];
    s[i + 1] = hex[(v >> 4) & 15];
  }
  return s;
}

Clock::time_point deadline(const OperationOptions& o) {
  if (o.wait_for_ack || o.timeout.count() < 0 || o.timeout > std::chrono::minutes(5))
    throw std::invalid_argument("Invalid stream operation options");
  return Clock::now() + (o.timeout.count() ? o.timeout : std::chrono::seconds(30));
}

void valid_string(const std::string& s, size_t max) {
  Utf8State utf8;
  if (s.size() > max || !utf8.append(bytes_of(s)) || utf8.remaining)
    throw std::invalid_argument("Invalid stream text or metadata");
}
}  // namespace

bool Utf8State::append(std::span<const uint8_t> bytes) {
  for (auto c : bytes) {
    if (!remaining) {
      if (c < 0x80)
        continue;
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
      } else
        return false;
    } else {
      if ((c & 0xc0) != 0x80)
        return false;
      value = (value << 6) | (c & 63);
      if (!--remaining &&
          (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)))
        return false;
    }
  }
  return true;
}

void StreamReaderState::finish(std::exception_ptr failure) {
  std::lock_guard lock(mutex);
  if (closed)
    return;
  closed = true;
  error = failure;
  if (failure) {
    chunks.clear();
    bytes = 0;
  }
  ready.notify_all();
}

StreamSession::~StreamSession() {
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

void StreamSession::fail_outgoing(const std::string& id, std::exception_ptr failure) {
  auto it = writers_.find(id);
  if (it == writers_.end())
    return;
  for (auto& b : it->second.batches)
    if (b->promise)
      b->promise->set_exception(failure);
  queued_ -= it->second.queued;
  writers_.erase(it);
}

void StreamSession::disconnect() {
  std::lock_guard lock(mutex_);
  connected_ = false;
  ++generation_;
  auto failure = error(ErrorCode::session_closed, "Data stream interrupted by connection change");
  while (!writers_.empty())
    fail_outgoing(writers_.begin()->first, failure);
  for (auto& [key, r] : readers_)
    r->finish(failure);
  readers_.clear();
  cursor_.clear();
}

void StreamSession::participant_left(const std::string& identity) {
  std::lock_guard lock(mutex_);
  for (auto it = readers_.begin(); it != readers_.end();)
    if (it->first.first == identity) {
      it->second->finish(error(ErrorCode::session_closed, "Stream sender left"));
      it = readers_.erase(it);
    } else
      ++it;
}

void StreamSession::register_handler(bool text, const std::string& topic, StreamHandler handler) {
  valid_string(topic, 1024);
  if (!handler)
    throw std::invalid_argument("Empty stream handler");
  std::lock_guard lock(mutex_);
  if (handlers_.size() >= 256)
    throw SDKError(ErrorDomain::data, ErrorCode::resource_exhausted, "Stream handler limit");
  if (!handlers_.emplace(std::make_pair(text, topic), std::move(handler)).second)
    throw std::invalid_argument("Duplicate stream handler");
}

void StreamSession::unregister_handler(bool text, const std::string& topic) {
  std::lock_guard lock(mutex_);
  handlers_.erase({text, topic});
}

std::shared_ptr<StreamWriter> StreamSession::open(bool text, StreamOptions o) {
  if (o.stream_id.empty())
    o.stream_id = new_id();
  valid_string(o.stream_id, 256);
  valid_string(o.topic, 1024);
  valid_string(o.name, 1024);
  valid_string(o.mime_type, 256);
  valid_string(o.reply_to_stream_id, 256);
  if (o.attributes.size() > 64 || o.destination_identities.size() > 64 ||
      o.attached_stream_ids.size() > 64)
    throw std::invalid_argument("Too much stream metadata");
  for (auto& [k, v] : o.attributes) {
    valid_string(k, 256);
    valid_string(v, 2048);
  }
  for (auto& s : o.destination_identities)
    valid_string(s, 256);
  for (auto& s : o.attached_stream_ids)
    valid_string(s, 256);
  if (o.operation < 0 || o.operation > 3 || o.version < 0)
    throw std::invalid_argument("Invalid text stream operation/version");
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
  for (auto& d : o.destination_identities)
    packet.add_destination_identities(d);
  auto* h = packet.mutable_stream_header();
  h->set_stream_id(info.id);
  h->set_timestamp(info.timestamp_ms);
  h->set_topic(info.topic);
  h->set_mime_type(info.mime_type);
  if (info.total_size)
    h->set_total_length(*info.total_size);
  for (auto& [k, v] : info.attributes)
    (*h->mutable_attributes())[k] = v;
  if (text) {
    auto* t = h->mutable_text_header();
    t->set_operation_type(static_cast<livekit::DataStream::OperationType>(o.operation));
    t->set_version(o.version);
    t->set_generated(o.generated);
    t->set_reply_to_stream_id(o.reply_to_stream_id);
    for (auto& id : o.attached_stream_ids)
      t->add_attached_stream_ids(id);
  } else
    h->mutable_byte_header()->set_name(info.name);
  const auto size = packet.ByteSizeLong();
  if (size > 14000)
    throw std::invalid_argument("Stream header exceeds packet limit");
  std::lock_guard lock(mutex_);
  if (!connected_)
    throw SDKError(ErrorDomain::data, ErrorCode::session_closed, "Room is disconnected");
  if (writers_.size() >= 32 || queued_ + size > session_limit)
    throw SDKError(ErrorDomain::data, ErrorCode::resource_exhausted, "Outgoing stream limit");
  if (writers_.contains(info.id))
    throw std::invalid_argument("Duplicate stream ID");
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
  return std::shared_ptr<StreamWriter>(
      new StreamWriter(weak_from_this(), generation_, std::move(info)));
}

std::future<void> StreamSession::enqueue(uint64_t generation, const std::string& id,
                                         std::span<const uint8_t> data,
                                         std::optional<std::string> reason,
                                         OperationOptions operation) {
  auto promise = std::make_shared<std::promise<void>>();
  auto future = promise->get_future();
  try {
    const auto end = deadline(operation);
    if (operation.stop.stop_requested())
      std::rethrow_exception(error(ErrorCode::cancelled, "Stream write cancelled"));
    if (reason)
      valid_string(*reason, 1024);
    if (data.size() > stream_limit)
      std::rethrow_exception(error(ErrorCode::resource_exhausted,
                                   "Stream write exceeds bounded queue; submit smaller blocks"));
    std::lock_guard lock(mutex_);
    auto it = writers_.find(id);
    if (!connected_ || generation != generation_ || it == writers_.end())
      std::rethrow_exception(error(ErrorCode::stale_handle, "Stream is closed or stale"));
    auto& out = it->second;
    if (out.closing)
      throw std::logic_error("Stream already closing");
    if (data.size() > std::numeric_limits<uint64_t>::max() - out.size)
      throw std::invalid_argument("Stream length overflow");
    const auto total = out.size + data.size();
    auto utf8 = out.utf8;
    if (out.info.text && (!utf8.append(data) || (reason && reason->empty() && utf8.remaining)))
      throw std::invalid_argument("Invalid UTF-8 stream content");
    if (out.info.total_size && (total > *out.info.total_size ||
                                (reason && reason->empty() && total != *out.info.total_size)))
      throw std::invalid_argument("Stream total length mismatch");
    auto batch = std::make_shared<Batch>();
    batch->promise = promise;
    batch->options = operation;
    batch->deadline = end;
    size_t size = 0;
    uint64_t index = out.index;
    for (size_t offset = 0; offset < data.size();) {
      size_t n = std::min(chunk_size, data.size() - offset);
      livekit::DataPacket p;
      p.set_kind(0);
      for (auto& d : out.destinations)
        p.add_destination_identities(d);
      auto* c = p.mutable_stream_chunk();
      c->set_stream_id(id);
      c->set_chunk_index(index++);
      c->set_content(data.data() + offset, n);
      if (p.ByteSizeLong() > 15000)
        throw std::invalid_argument("Stream routing metadata exceeds packet limit");
      size += p.ByteSizeLong();
      batch->packets.push_back(std::move(p));
      offset += n;
    }
    if (reason) {
      livekit::DataPacket p;
      p.set_kind(0);
      for (auto& d : out.destinations)
        p.add_destination_identities(d);
      auto* t = p.mutable_stream_trailer();
      t->set_stream_id(id);
      t->set_reason(*reason);
      size += p.ByteSizeLong();
      batch->packets.push_back(std::move(p));
    }
    if (out.queued + size > stream_limit || queued_ + size > session_limit)
      std::rethrow_exception(error(ErrorCode::resource_exhausted, "Stream queue is full"));
    if (batch->packets.empty()) {
      promise->set_value();
      return future;
    }
    out.index = index;
    out.size = total;
    out.utf8 = utf8;
    out.closing = reason.has_value();
    out.queued += size;
    queued_ += size;
    out.touched = Clock::now();
    out.batches.push_back(batch);
  } catch (...) {
    promise->set_exception(std::current_exception());
  }
  return future;
}

void StreamSession::expire() {
  const auto now = Clock::now();
  for (auto it = writers_.begin(); it != writers_.end();) {
    auto current = it++;
    for (auto& b : current->second.batches)
      if (b->options.stop.stop_requested() || now >= b->deadline) {
        fail_outgoing(current->first,
                      error(b->sent ? ErrorCode::outcome_unknown
                                    : (b->options.stop.stop_requested() ? ErrorCode::cancelled
                                                                        : ErrorCode::timeout),
                            "Stream operation interrupted; stream is no longer writable"));
        current = writers_.end();
        break;
      }
    if (current != writers_.end() && now - current->second.touched > std::chrono::seconds(60))
      fail_outgoing(current->first, error(ErrorCode::timeout, "Outgoing stream idle timeout"));
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
      if (expired)
        r->finish(error(ErrorCode::timeout, "Incoming stream idle timeout"));
      it = readers_.erase(it);
    } else
      ++it;
  }
}

bool StreamSession::has_outgoing() {
  std::lock_guard lock(mutex_);
  expire();
  return connected_ && queued_ != 0;
}

void StreamSession::pump(const std::function<bool(const livekit::DataPacket&)>& send,
                         size_t max_packets, const std::function<bool()>& ready) {
  {
    std::lock_guard lock(mutex_);
    expire();
  }
  for (size_t n = 0; n < max_packets; ++n) {
    if (ready && !ready())
      return;
    std::string id;
    livekit::DataPacket packet;
    std::shared_ptr<Batch> batch;
    uint64_t generation;
    {
      std::lock_guard lock(mutex_);
      expire();
      if (!connected_ || !queued_)
        return;
      auto it = writers_.upper_bound(cursor_);
      for (size_t tries = 0; tries < writers_.size(); ++tries) {
        if (it == writers_.end())
          it = writers_.begin();
        if (!it->second.batches.empty())
          break;
        ++it;
      }
      if (it == writers_.end() || it->second.batches.empty())
        return;
      id = it->first;
      cursor_ = id;
      batch = it->second.batches.front();
      packet = batch->packets.front();
      batch->sent = true;
      generation = generation_;
    }
    bool ok = false;
    try {
      ok = send(packet);
    } catch (...) {
    }
    std::lock_guard lock(mutex_);
    auto it = writers_.find(id);
    if (generation != generation_ || it == writers_.end())
      continue;
    if (!ok) {
      fail_outgoing(id, error(ErrorCode::outcome_unknown, "Stream channel submission failed"));
      continue;
    }
    auto& out = it->second;
    const auto size = batch->packets.front().ByteSizeLong();
    out.queued -= size;
    queued_ -= size;
    batch->packets.pop_front();
    out.touched = Clock::now();
    if (batch->packets.empty()) {
      if (batch->promise)
        batch->promise->set_value();
      out.batches.pop_front();
    }
    if (out.closing && out.batches.empty())
      writers_.erase(it);
  }
}

void StreamSession::receive(const livekit::DataPacket& packet) {
  StreamHandler handler;
  std::shared_ptr<StreamReaderState> reader;
  const auto& sender = packet.participant_identity();
  if (sender.empty() || packet.ByteSizeLong() > 16384)
    return;
  {
    std::lock_guard lock(mutex_);
    if (!connected_)
      return;
    expire();
    if (packet.has_stream_header()) {
      const auto& h = packet.stream_header();
      const auto key = std::make_pair(sender, h.stream_id());
      if (h.stream_id().empty() || h.stream_id().size() > 256 || readers_.contains(key) ||
          readers_.size() >= 32 || h.compression() < livekit::DataStream::NONE ||
          h.compression() > livekit::DataStream::DEFLATE_RAW)
        return;
      const bool text = h.has_text_header();
      if (!text && !h.has_byte_header())
        return;
      if (text && (h.text_header().operation_type() < 0 || h.text_header().operation_type() > 3 ||
                   h.text_header().version() < 0))
        return;
      auto found = handlers_.find({text, h.topic()});
      if (found == handlers_.end())
        return;
      reader = std::make_shared<StreamReaderState>();
      if (h.compression() == livekit::DataStream::DEFLATE_RAW)
        reader->inflater = std::make_unique<InflateRaw>();
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
      if (h.has_total_length())
        i.total_size = h.total_length();
      if (text) {
        i.reply_to_stream_id = h.text_header().reply_to_stream_id();
        i.attached_stream_ids = {h.text_header().attached_stream_ids().begin(),
                                 h.text_header().attached_stream_ids().end()};
      } else
        i.name = h.byte_header().name();
      if (h.has_inline_content()) {
        std::string bytes;
        try {
          bytes = reader->inflater
                      ? reader->inflater->append(bytes_of(h.inline_content()), reader_limit, true)
                      : h.inline_content();
        } catch (...) {
          return;
        }
        if ((i.total_size && *i.total_size != bytes.size()) ||
            (text && (!reader->utf8.append(bytes_of(bytes)) || reader->utf8.remaining)))
          return;
        reader->chunks.emplace_back(bytes.begin(), bytes.end());
        reader->bytes = bytes.size();
        reader->received = bytes.size();
        reader->closed = true;
      } else
        readers_.emplace(key, reader);
      handler = found->second;
    } else {
      const auto id = packet.has_stream_chunk()     ? packet.stream_chunk().stream_id()
                      : packet.has_stream_trailer() ? packet.stream_trailer().stream_id()
                                                    : std::string{};
      auto it = readers_.find({sender, id});
      if (it == readers_.end())
        return;
      reader = it->second;
      std::lock_guard rlock(reader->mutex);
      if (reader->closed)
        return;
      auto fail = [&](ErrorCode code, const char* message) {
        reader->closed = true;
        reader->error = error(code, message);
        reader->chunks.clear();
        reader->bytes = 0;
      };
      if (packet.has_stream_chunk()) {
        const auto& c = packet.stream_chunk();
        std::string content;
        try {
          content = reader->inflater ? reader->inflater->append(bytes_of(c.content()), reader_limit)
                                     : c.content();
        } catch (...) {
          fail(ErrorCode::malformed_message, "Invalid compressed stream chunk");
          reader->ready.notify_all();
          readers_.erase(it);
          return;
        }
        const auto size = content.size();
        if (c.chunk_index() != reader->index || c.version() != 0 || size > reader_limit ||
            size > std::numeric_limits<uint64_t>::max() - reader->received)
          fail(ErrorCode::malformed_message, "Invalid stream chunk sequence");
        else if (reader->bytes + size > reader_limit)
          fail(ErrorCode::resource_exhausted, "Stream reader is too slow");
        else if (reader->info.total_size && reader->received + size > *reader->info.total_size)
          fail(ErrorCode::malformed_message, "Stream exceeds declared size");
        else if (reader->info.text && !reader->utf8.append(bytes_of(content)))
          fail(ErrorCode::malformed_message, "Invalid stream UTF-8");
        else {
          ++reader->index;
          reader->received += size;
          reader->bytes += size;
          reader->chunks.emplace_back(content.begin(), content.end());
        }
      } else {
        const auto& t = packet.stream_trailer();
        if (reader->inflater && t.reason().empty())
          try {
            reader->inflater->append({}, reader_limit, true);
          } catch (...) {
            fail(ErrorCode::malformed_message, "Truncated compressed stream");
            reader->ready.notify_all();
            readers_.erase(it);
            return;
          }
        if (!t.reason().empty())
          fail(ErrorCode::request_failed, "Remote stream ended abnormally");
        else if ((reader->info.total_size && reader->received != *reader->info.total_size) ||
                 (reader->info.text && reader->utf8.remaining))
          fail(ErrorCode::malformed_message, "Truncated stream");
        else {
          reader->closed = true;
          for (auto& [k, v] : t.attributes())
            reader->info.attributes[k] = v;
        }
      }
      reader->touched = Clock::now();
      reader->ready.notify_all();
      if (reader->closed)
        readers_.erase(it);
      return;
    }
  }
  auto public_reader = std::shared_ptr<StreamReader>(new StreamReader(reader));
  if (!handlers_executor_.post([handler = std::move(handler), public_reader, reader] {
        try {
          handler(public_reader);
        } catch (...) {
          reader->finish(std::current_exception());
        }
      }))
    reader->finish(error(ErrorCode::resource_exhausted, "Stream handler queue is full"));
}

std::future<StreamInfo> StreamSession::send_text(std::string text, StreamOptions options,
                                                 OperationOptions operation) {
  valid_string(text, 16 * 1024 * 1024);
  const auto end = deadline(operation);
  options.total_size = text.size();
  auto writer = open(true, std::move(options));
  auto promise = std::make_shared<std::promise<StreamInfo>>();
  auto result = promise->get_future();
  if (!send_executor_.post([writer, text = std::move(text), operation, promise, end] {
        try {
          auto remaining = [&] {
            auto next = operation;
            if (Clock::now() >= end)
              std::rethrow_exception(
                  error(ErrorCode::timeout, "Stream operation deadline exceeded"));
            next.timeout = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now());
            return next;
          };
          for (size_t at = 0; at < text.size(); at += chunk_size)
            writer
                ->write(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data() + at),
                                                 std::min(chunk_size, text.size() - at)),
                        remaining())
                .get();
          writer->close({}, remaining()).get();
          promise->set_value(writer->info());
        } catch (...) {
          try {
            writer->close("error");
          } catch (...) {
          }
          promise->set_exception(std::current_exception());
        }
      })) {
    writer->close("error");
    promise->set_exception(error(ErrorCode::resource_exhausted, "Stream sender queue is full"));
  }
  return result;
}

std::future<StreamInfo> StreamSession::send_file(std::filesystem::path path, StreamOptions options,
                                                 OperationOptions operation) {
  const auto end = deadline(operation);
  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!*file)
    throw std::runtime_error("Cannot open stream file");
  file->seekg(0, std::ios::end);
  auto size = file->tellg();
  if (size < 0)
    throw std::runtime_error("Cannot determine file length");
  file->seekg(0);
  options.total_size = static_cast<uint64_t>(size);
  if (options.name.empty()) {
    auto u8 = path.filename().u8string();
    options.name.assign(u8.begin(), u8.end());
  }
  auto writer = open(false, std::move(options));
  auto promise = std::make_shared<std::promise<StreamInfo>>();
  auto result = promise->get_future();
  if (!send_executor_.post([writer, file, operation, promise, end] {
        try {
          auto remaining = [&] {
            auto next = operation;
            if (Clock::now() >= end)
              std::rethrow_exception(
                  error(ErrorCode::timeout, "Stream operation deadline exceeded"));
            next.timeout = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now());
            return next;
          };
          std::array<uint8_t, chunk_size> buffer;
          while (file->read(reinterpret_cast<char*>(buffer.data()), buffer.size()) ||
                 file->gcount())
            writer->write({buffer.data(), static_cast<size_t>(file->gcount())}, remaining()).get();
          if (file->bad())
            throw std::runtime_error("File read failed");
          writer->close({}, remaining()).get();
          promise->set_value(writer->info());
        } catch (...) {
          try {
            writer->close("error");
          } catch (...) {
          }
          promise->set_exception(std::current_exception());
        }
      })) {
    writer->close("error");
    promise->set_exception(error(ErrorCode::resource_exhausted, "Stream sender queue is full"));
  }
  return result;
}
}  // namespace livekit::sdk::detail

namespace livekit::sdk {
StreamWriter::~StreamWriter() {
  try {
    close("interrupted");
  } catch (...) {
  }
}

StreamInfo StreamReader::info() const {
  std::lock_guard lock(state_->mutex);
  return state_->info;
}

std::optional<std::vector<uint8_t>> StreamReader::read(std::chrono::milliseconds timeout,
                                                       std::stop_token stop) {
  if (timeout.count() < 0)
    throw std::invalid_argument("Negative read timeout");
  std::unique_lock lock(state_->mutex);
  const bool ready = state_->ready.wait_for(
      lock, stop, timeout, [&] { return state_->closed || !state_->chunks.empty(); });
  if (stop.stop_requested())
    throw SDKError(ErrorDomain::data, ErrorCode::cancelled, "Stream read cancelled");
  if (!ready)
    throw SDKError(ErrorDomain::data, ErrorCode::timeout, "Stream read timeout");
  if (state_->error)
    std::rethrow_exception(state_->error);
  if (state_->chunks.empty())
    return {};
  auto result = std::move(state_->chunks.front());
  state_->chunks.pop_front();
  state_->bytes -= result.size();
  return result;
}

std::vector<uint8_t> StreamReader::read_all(size_t max, std::stop_token stop) {
  std::vector<uint8_t> result;
  while (auto chunk = read(std::chrono::seconds(30), stop)) {
    if (chunk->size() > max - result.size()) {
      close();
      throw SDKError(ErrorDomain::data, ErrorCode::resource_exhausted,
                     "Stream read_all limit exceeded");
    }
    result.insert(result.end(), chunk->begin(), chunk->end());
  }
  return result;
}

std::string StreamReader::read_all_text(size_t max, std::stop_token stop) {
  if (!info().text)
    throw std::logic_error("Not a text stream");
  auto bytes = read_all(max, stop);
  return {bytes.begin(), bytes.end()};
}

void StreamReader::close() {
  state_->finish(std::make_exception_ptr(
      SDKError(ErrorDomain::data, ErrorCode::cancelled, "Stream reader closed")));
}

std::future<void> StreamWriter::write(std::span<const uint8_t> bytes,
                                      const OperationOptions& operation) {
  auto session = session_.lock();
  if (!session)
    throw SDKError(ErrorDomain::data, ErrorCode::session_closed, "Room destroyed");
  return session->enqueue(generation_, info_.id, bytes, {}, operation);
}

std::future<void> StreamWriter::write(const std::string& text, const OperationOptions& operation) {
  return write({reinterpret_cast<const uint8_t*>(text.data()), text.size()}, operation);
}

std::future<void> StreamWriter::close(const std::string& reason,
                                      const OperationOptions& operation) {
  auto session = session_.lock();
  if (!session)
    throw SDKError(ErrorDomain::data, ErrorCode::session_closed, "Room destroyed");
  return session->enqueue(generation_, info_.id, {}, reason, operation);
}
}  // namespace livekit::sdk
