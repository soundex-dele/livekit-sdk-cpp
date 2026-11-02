#include "backends/rtc/livekit/service_result.hpp"
#include "backends/rtc/livekit/room_service_state.hpp"
#include "backends/rtc/livekit/data_message_wire.hpp"
#include "livekit/result.hpp"
#include <algorithm>
#include <utility>

namespace livekit::sdk::rtc::detail {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t max_packets = 256, max_bytes = 4 * 1024 * 1024;

Failure failure(ErrorCode code, const char* message) {
  return make_failure(ErrorDomain::lifecycle, code, message);
}

size_t size(const DataEvent& event) {
  size_t result = event.sender_identity.size();
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ReceivedData>) {
          result += value.topic.size() + value.bytes.size();
        } else if constexpr (std::is_same_v<T, Transcription>) {
          result += value.participant_identity.size() + value.track_id.size();
          for (const auto& s : value.segments) {
            result += s.id.size() + s.text.size() + s.language.size();
          }
        } else if constexpr (std::is_same_v<T, DataServiceError>) {
          result += value.message.size();
        } else {
          result += value.digit.size();
        }
      },
      event.value);
  return result;
}
}  // namespace

std::shared_ptr<RoomServices> RoomServiceState::handle() {
  return std::shared_ptr<RoomServices>(new RoomServices(shared_from_this()));
}

Result<void> RoomServiceState::configure(EncryptionOptions options,
                                         std::chrono::milliseconds operation_timeout) {
  std::lock_guard lock(mutex);
  if (active || closed) {
    return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state,
                                              "Cannot configure an active/closed room service"));
  }
  encryption = std::move(options);
  timeout = operation_timeout;
  return tracks->configure(encryption);
}

Result<void> RoomServiceState::attach_wakeup(
    std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup) {
  if (!wakeup) {
    return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_argument,
                                              "Room services require a wake source"));
  }
  {
    std::lock_guard lock(mutex);
    if (active || closed || !outgoing.empty()) {
      return Result<void>::failure(
          make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state,
                       "Attach room service wake source before connecting"));
    }
    cancellations = std::make_shared<livekit::sdk::detail::ReadySet<uint64_t>>(wakeup, max_packets);
    work_wakeup = wakeup;
  }
  rpc.attach_wakeup(std::move(wakeup));
  return Result<void>::success();
}

std::optional<Clock::time_point> RoomServiceState::next_data_deadline() const {
  std::lock_guard lock(mutex);
  return send_deadlines.next();
}

void RoomServiceState::erase_send_locked(std::map<uint64_t, Pending>::iterator position) {
  outgoing_bytes -= position->second.bytes;
  send_deadlines.erase(position->first);
  cancellations->forget(position->first);
  outgoing.erase(position);
}

void RoomServiceState::expire_sends_locked() {
  auto ready = cancellations->take();
  const auto now = Clock::now();
  for (const auto id : send_deadlines.take_due(now)) {
    ready.insert(id);
  }
  for (const auto id : ready) {
    const auto found = outgoing.find(id);
    if (found == outgoing.end()) {
      continue;
    }
    const auto& pending = found->second;
    if (pending.stop.stop_requested()) {
      pending.promise->set_failure(failure(ErrorCode::cancelled, "Data send cancelled"));
    } else if (now >= pending.deadline) {
      pending.promise->set_failure(failure(ErrorCode::timeout, "Data send timed out"));
    } else {
      continue;
    }
    erase_send_locked(found);
  }
}

Result<void> RoomServiceState::connect() {
  {
    std::lock_guard lock(mutex);
    if (closed) {
      return Result<void>::failure(
          make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state, "Room services closed"));
    }
    active = true;
  }
  rpc.connect();
  streams->connect();
  tracks->connect();
  return Result<void>::success();
}

void RoomServiceState::interrupt() {
  {
    std::lock_guard lock(mutex);
    if (!active) {
      return;
    }
    active = false;
    for (auto& [id, pending] : outgoing) {
      pending.promise->set_failure(failure(ErrorCode::connection_lost, "Data send interrupted"));
    }
    send_deadlines.clear();
    cancellations->clear();
    outgoing.clear();
    work_wakeup->signal();
    incoming.clear();
    outgoing_bytes = incoming_bytes = 0;
    changed.notify_all();
  }
  rpc.disconnect();
  streams->disconnect();
  tracks->rejoin();
  rpc_pending.clear();
}

void RoomServiceState::close() {
  std::shared_ptr<Handler> retired;
  {
    std::lock_guard lock(mutex);
    if (closed) {
      return;
    }
    closed = true;
    active = false;
    retired = std::move(handler);
    for (auto& [id, pending] : outgoing) {
      pending.promise->set_failure(failure(ErrorCode::session_closed, "Room services closed"));
    }
    send_deadlines.clear();
    cancellations->clear();
    outgoing.clear();
    work_wakeup->signal();
    incoming.clear();
    outgoing_bytes = incoming_bytes = 0;
    changed.notify_all();
  }
  rpc.disconnect();
  streams->disconnect();
  tracks->disconnect();
  rpc_pending.clear();
}

void RoomServiceState::participant(const livekit::ParticipantInfo& info) {
  tracks->participant(info);
  if (info.state() == 3) {
    rpc.participant_left(info.identity());
    streams->participant_left(info.identity());
  }
}

Result<void> RoomServiceState::receive_signal(const livekit::SignalResponse& response) {
  return tracks->receive_signal(response);
}

std::future<Result<void>> RoomServiceState::enqueue(livekit::DataPacket packet,
                                                    DataSendOptions options) {
  auto promise = std::make_shared<livekit::sdk::detail::ServicePromise<void>>();
  auto result = promise->get_future();
  auto submitted = [&]() -> Result<void> {
    if (options.operation.wait_for_ack || options.operation.timeout.count() < 0 ||
        options.operation.timeout > std::chrono::minutes(5)) {
      return Result<void>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                                "Invalid data send operation"));
    }
    if (options.destinations.size() > 128) {
      return Result<void>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                                "Too many data destinations"));
    }
    for (const auto& identity : options.destinations) {
      if (identity.empty() || identity.size() > 1024 ||
          !livekit::sdk::detail::valid_utf8(identity)) {
        return Result<void>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                                  "Invalid data destination"));
      }
      packet.add_destination_identities(identity);
    }
    packet.set_kind(options.reliable ? 0 : 1);
    const auto bytes = packet.ByteSizeLong();
    // Leave room for the authenticated encryption envelope and server identity.
    if (bytes > 15000) {
      return Result<void>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                                "Data packet exceeds 15000 byte limit"));
    }
    std::lock_guard lock(mutex);
    if (!active || closed) {
      return Result<void>::failure(
          failure(ErrorCode::session_closed, "Room services are not connected"));
    }
    if (options.operation.stop.stop_requested()) {
      return Result<void>::failure(failure(ErrorCode::cancelled, "Data send cancelled"));
    }
    if (outgoing.size() >= max_packets || bytes > max_bytes - outgoing_bytes) {
      return Result<void>::failure(
          failure(ErrorCode::resource_exhausted, "Data send queue exhausted"));
    }
    const auto id = ++next_send;
    const auto deadline =
        Clock::now() + (options.operation.timeout.count() ? options.operation.timeout : timeout);
    {
      const auto watched = cancellations->watch(id);
      if (!watched) {
        return Result<void>::failure(watched.failure_record());
      }
      const auto wake = *watched;
      auto cancellation = std::make_shared<std::stop_callback<std::function<void()>>>(
          options.operation.stop, [wake] { wake->signal(); });
      send_deadlines.set(id, deadline);
      outgoing.emplace(id, Pending{std::move(packet), bytes, promise, deadline,
                                   options.operation.stop, std::move(cancellation)});
      outgoing_bytes += bytes;
      work_wakeup->signal();
    }
    return Result<void>::success();
  }();
  if (!submitted) {
    promise->set_failure(submitted.failure_record());
  }
  return result;
}

Result<void> RoomServiceState::emit(DataEvent event) {
  const auto bytes = size(event);
  std::lock_guard lock(mutex);
  if (!active || closed) {
    return Result<void>::success();
  }
  if (incoming.size() >= max_packets || bytes > max_bytes - incoming_bytes) {
    if (!event.reliable) {
      ++dropped_events;
      return Result<void>::success();
    }
    const auto error = make_failure(ErrorDomain::data, ErrorCode::resource_exhausted,
                                    "Reliable data receive queue exhausted");
    callback_failure = error;
    return Result<void>::failure(error);
  }
  incoming.push_back(std::move(event));
  incoming_bytes += bytes;
  return schedule_events_locked();
}

Result<void> RoomServiceState::set_dispatcher(Dispatch value,
                                              std::function<void(Failure)> failure) {
  std::lock_guard lock(mutex);
  if (active || closed || dispatch) {
    return Result<void>::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_state,
                     "Install room service notifications once, before connect"));
  }
  dispatch = std::move(value);
  report_failure = std::move(failure);
  return Result<void>::success();
}

Result<void> RoomServiceState::set_event_handler(
    std::function<Result<void>(const DataEvent&)> value) {
  auto next = value ? std::make_shared<Handler>() : nullptr;
  if (next) {
    next->call = std::move(value);
  }
  std::unique_lock lock(mutex);
  if (next && (!dispatch || closed || callback_failure)) {
    return Result<void>::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::session_closed,
                                              "Room service notification delivery is unavailable"));
  }
  auto previous = std::exchange(handler, std::move(next));
  auto executing = in_flight;
  if (!handler) {
    incoming.clear();
    incoming_bytes = 0;
  }
  auto scheduled = schedule_events_locked();
  if (executing && executing->thread != std::this_thread::get_id()) {
    changed.wait(lock, [&] { return !executing->running; });
  }
  lock.unlock();
  return scheduled;
}

RoomServiceSnapshot RoomServiceState::snapshot() const {
  std::lock_guard lock(mutex);
  return {
      active,         closed,           bool(handler),   incoming.size(), incoming_bytes,
      dropped_events, callback_failure, outgoing.size(), outgoing_bytes,  send_deadlines.size()};
}

Result<void> RoomServiceState::schedule_events_locked() {
  if (notification_scheduled || !handler || incoming.empty() || !active || closed) {
    return Result<void>::success();
  }
  notification_scheduled = true;
  auto scheduled = dispatch([weak = weak_from_this()] {
    if (auto state = weak.lock()) {
      state->deliver_events();
    }
  });
  if (!scheduled) {
    notification_scheduled = false;
  }
  return scheduled;
}

void RoomServiceState::deliver_events() {
  // A bounded batch yields to room/graph notifications sharing this queue.
  for (size_t count = 0; count < 32; ++count) {
    std::shared_ptr<Handler> target;
    std::optional<DataEvent> event;
    {
      std::lock_guard lock(mutex);
      if (!handler || incoming.empty() || !active || closed) {
        break;
      }
      target = in_flight = handler;
      target->running = true;
      target->thread = std::this_thread::get_id();
      event = std::move(incoming.front());
      incoming.pop_front();
      incoming_bytes -= size(*event);
    }
    Failure error;
    std::shared_ptr<Handler> retired;
    auto delivered = target->call(*event);
    if (!delivered) {
      error = with_context(delivered.failure_record(), {"data_callback"});
    }
    {
      std::lock_guard lock(mutex);
      target->running = false;
      target->thread = {};
      in_flight.reset();
      if (error) {
        callback_failure = error;
        retired = std::move(handler);
        incoming.clear();
        incoming_bytes = 0;
      }
      changed.notify_all();
    }
    if (error) {
      if (report_failure) {
        report_failure(error);
      }
      break;
    }
  }
  Failure failed;
  {
    std::lock_guard lock(mutex);
    notification_scheduled = false;
    auto scheduled = schedule_events_locked();
    if (!scheduled) {
      callback_failure = failed = scheduled.failure_record();
      handler.reset();
      incoming.clear();
      incoming_bytes = 0;
    }
  }
  if (failed && report_failure) {
    report_failure(failed);
  }
}

Result<void> RoomServiceState::receive(const std::string& label, std::span<const uint8_t> bytes) {
  const auto reject_packet = [&](Failure failure, std::string sender = {}) {
    return emit(
        {std::move(sender), label == "_reliable",
         DataServiceError{failure->message, with_context(std::move(failure), {"receive_data"})}});
  };
  if (label == "_data_track") {
    auto received = tracks->receive_packet(bytes);
    if (!received && (received.error().code == ErrorCode::permission_denied ||
                      received.error().code == ErrorCode::malformed_message ||
                      received.error().code == ErrorCode::invalid_argument ||
                      received.error().code == ErrorCode::unsupported)) {
      return reject_packet(received.failure_record());
    }
    return received;
  }
  if (bytes.size() > 16384) {
    return reject_packet(make_failure(ErrorDomain::data, ErrorCode::malformed_message,
                                      "Data packet exceeds channel limit"));
  }
  livekit::DataPacket packet;
  if (!packet.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
    return reject_packet(
        make_failure(ErrorDomain::data, ErrorCode::malformed_message, "Malformed data packet"));
  }
  auto decrypted = livekit::sdk::detail::decrypt_packet(packet, encryption);
  if (!decrypted) {
    return emit({packet.participant_identity(), label == "_reliable",
                 DataServiceError{"Data authentication failed",
                                  with_context(decrypted.failure_record(), {"decrypt_data"})}});
  }
  packet = std::move(*decrypted);
  if (packet.has_rpc_request() || packet.has_rpc_ack() || packet.has_rpc_response()) {
    rpc.receive(packet);
    return Result<void>::success();
  }
  if (packet.has_stream_header() || packet.has_stream_chunk() || packet.has_stream_trailer()) {
    streams->receive(packet);
    return Result<void>::success();
  }
  DataEvent event;
  event.sender_identity = packet.participant_identity();
  event.reliable = label == "_reliable";
  if (packet.has_user()) {
    if (event.sender_identity.empty()) {
      event.sender_identity = packet.user().participant_identity();
    }
    const auto& data = packet.user().payload();
    event.value = ReceivedData{packet.user().topic(), {data.begin(), data.end()}};
  } else if (packet.has_sip_dtmf()) {
    DtmfDigit value{packet.sip_dtmf().code(), packet.sip_dtmf().digit()};
    if (auto valid = livekit::sdk::detail::validate_dtmf(value); !valid) {
      return reject_packet(valid.failure_record(), event.sender_identity);
    }
    event.value = std::move(value);
  } else if (packet.has_transcription()) {
    auto json = livekit::sdk::detail::transcription_json(packet.transcription());
    if (!json) {
      return reject_packet(json.failure_record(), event.sender_identity);
    }
    auto parsed = livekit::sdk::detail::parse_transcription(*json);
    if (!parsed) {
      return reject_packet(parsed.failure_record(), event.sender_identity);
    }
    event.value = std::move(*parsed);
  } else {
    return Result<void>::success();
  }
  return emit(std::move(event));
}

Result<bool> RoomServiceState::send_packet(const livekit::DataPacket& packet, const Send& send) {
  auto encrypted = livekit::sdk::detail::encrypt_packet(packet, encryption);
  if (!encrypted) {
    return Result<bool>::failure(encrypted.failure_record());
  }
  const auto wire = encrypted->SerializeAsString();
  if (wire.size() > 16384) {
    return Result<bool>::failure(make_failure(ErrorDomain::data, ErrorCode::invalid_argument,
                                              "Encrypted data packet exceeds channel limit"));
  }
  return Result<bool>::success(send(packet.kind() == 0 ? "_reliable" : "_lossy",
                                    {reinterpret_cast<const uint8_t*>(wire.data()), wire.size()}));
}

Result<void> RoomServiceState::pump(
    const Send& send, const std::function<Result<void>(const livekit::SignalRequest&)>& signal,
    const std::function<bool()>& reliable_ready) {
  {
    std::lock_guard lock(mutex);
    if (!active || closed) {
      return Result<void>::success();
    }
    // Cancellation and expiry must progress behind a backpressured FIFO head.
    expire_sends_locked();
  }
  bool data_blocked = false;
  for (int i = 0; i < 4; ++i) {
    std::optional<Pending> pending;
    {
      std::lock_guard lock(mutex);
      if (outgoing.empty()) {
        break;
      }
      pending = outgoing.begin()->second;
    }
    Failure error;
    if (pending->stop.stop_requested()) {
      error = failure(ErrorCode::cancelled, "Data send cancelled");
    } else if (Clock::now() >= pending->deadline) {
      error = failure(ErrorCode::timeout, "Data send timed out");
    } else {
      auto sent = send_packet(pending->packet, send);
      if (!sent) {
        error = sent.failure_record();
      } else if (!*sent) {
        data_blocked = true;
        break;
      }
    }
    std::lock_guard lock(mutex);
    if (outgoing.empty() || outgoing.begin()->second.promise != pending->promise) {
      continue;
    }
    if (error) {
      pending->promise->set_failure(error);
    } else {
      pending->promise->set_value();
    }
    erase_send_locked(outgoing.begin());
  }
  {
    std::lock_guard lock(mutex);
    // A bounded send quantum yields to native work, then continues without a timer.
    // Capacity notifications, cancellations and deadlines wake a blocked sender.
    if (!data_blocked && !outgoing.empty()) {
      work_wakeup->signal();
    }
  }
  if (rpc_pending.empty()) {
    for (auto& packet : rpc.take_packets(4)) {
      rpc_pending.push_back(std::move(packet));
    }
  }
  while (!rpc_pending.empty()) {
    if (!rpc.sendable(rpc_pending.front())) {
      rpc_pending.pop_front();
      continue;
    }
    auto sent = send_packet(rpc_pending.front(), send);
    if (!sent) {
      rpc.send_failed(rpc_pending.front());
    } else if (!*sent) {
      break;
    }
    rpc_pending.pop_front();
  }
  if (rpc_pending.empty() && rpc.has_outgoing()) {
    work_wakeup->signal();
  }
  streams->pump([&](const auto& packet) { return send_packet(packet, send); }, 4, reliable_ready);
  auto signals = tracks->take_signals();
  if (!signals) {
    return Result<void>::failure(signals.failure_record());
  }
  for (const auto& request : *signals) {
    if (auto sent = signal(request); !sent) {
      return sent;
    }
  }
  for (int i = 0; i < 4; ++i) {
    auto packet = tracks->take_packet();
    if (!packet) {
      return Result<void>::failure(packet.failure_record());
    }
    if (!*packet) {
      break;
    }
    tracks->packet_sent(**packet, send("_data_track", **packet));
  }
  return Result<void>::success();
}

}  // namespace livekit::sdk::rtc::detail

namespace livekit::sdk::rtc {
std::future<Result<void>> RoomServices::send_data(std::vector<uint8_t> bytes,
                                                  DataSendOptions options) {
  if (options.topic.size() > 256 || !livekit::sdk::detail::valid_utf8(options.topic)) {
    return livekit::sdk::detail::failed_service<void>(
        make_failure(ErrorDomain::data, ErrorCode::invalid_argument, "Invalid data topic"));
  }
  livekit::DataPacket packet;
  packet.mutable_user()->set_payload(bytes.data(), bytes.size());
  packet.mutable_user()->set_topic(options.topic);
  return state_->enqueue(std::move(packet), std::move(options));
}

std::future<Result<void>> RoomServices::send_dtmf(DtmfDigit value, DataSendOptions options) {
  auto packet = livekit::sdk::detail::encode_dtmf(value);
  if (!packet) {
    return livekit::sdk::detail::failed_service<void>(packet.failure_record());
  }
  return state_->enqueue(std::move(*packet), std::move(options));
}

std::future<Result<void>> RoomServices::send_transcription(Transcription value,
                                                           DataSendOptions options) {
  auto packet = livekit::sdk::detail::encode_transcription(value);
  if (!packet) {
    return livekit::sdk::detail::failed_service<void>(packet.failure_record());
  }
  return state_->enqueue(std::move(*packet), std::move(options));
}

Result<void> RoomServices::set_event_handler(
    std::function<Result<void>(const DataEvent&)> handler) {
  return state_->set_event_handler(std::move(handler));
}

RoomServiceSnapshot RoomServices::snapshot() const {
  return state_->snapshot();
}

Result<void> RoomServices::register_rpc(std::string method, RpcHandler handler) {
  return state_->rpc.register_method(method, std::move(handler));
}

void RoomServices::unregister_rpc(const std::string& method) {
  state_->rpc.unregister_method(method);
}

std::future<Result<std::string>> RoomServices::call_rpc(RpcCallOptions options) {
  return state_->rpc.call(options);
}

Result<void> RoomServices::register_stream(bool text, std::string topic, StreamHandler handler) {
  return state_->streams->register_handler(text, topic, std::move(handler));
}

void RoomServices::unregister_stream(bool text, const std::string& topic) {
  state_->streams->unregister_handler(text, topic);
}

Result<std::shared_ptr<StreamWriter>> RoomServices::open_stream(bool text, StreamOptions options) {
  return state_->streams->open(text, std::move(options));
}

std::future<Result<StreamInfo>> RoomServices::send_text(std::string value, StreamOptions options,
                                                        OperationOptions operation) {
  return state_->streams->send_text(std::move(value), std::move(options), operation);
}

std::future<Result<StreamInfo>> RoomServices::send_file(std::filesystem::path path,
                                                        StreamOptions options,
                                                        OperationOptions operation) {
  return state_->streams->send_file(std::move(path), std::move(options), operation);
}

std::future<Result<LocalDataTrack>> RoomServices::publish_data_track(std::string name,
                                                                     DataTrackOptions options,
                                                                     OperationOptions operation) {
  return state_->tracks->publish(std::move(name), std::move(options), operation);
}

std::vector<LocalDataTrack> RoomServices::local_data_tracks() const {
  return state_->tracks->local_tracks();
}

std::vector<RemoteDataTrack> RoomServices::remote_data_tracks() const {
  return state_->tracks->remote_tracks();
}
}  // namespace livekit::sdk::rtc
