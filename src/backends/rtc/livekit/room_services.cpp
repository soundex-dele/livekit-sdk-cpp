#include "backends/rtc/livekit/room_service_state.hpp"
#include "backends/rtc/livekit/data_message_wire.hpp"
#include "livekit/error.hpp"
#include <algorithm>
#include <utility>

namespace livekit::sdk::rtc::detail {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t max_packets = 256, max_bytes = 4 * 1024 * 1024;

std::exception_ptr failure(ErrorCode code, const char* message) {
  return std::make_exception_ptr(SDKError(ErrorDomain::lifecycle, code, message));
}

size_t size(const DataEvent& event) {
  size_t result = event.sender_identity.size();
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ReceivedData>)
          result += value.topic.size() + value.bytes.size();
        else if constexpr (std::is_same_v<T, Transcription>) {
          result += value.participant_identity.size() + value.track_id.size();
          for (const auto& s : value.segments)
            result += s.id.size() + s.text.size() + s.language.size();
        } else if constexpr (std::is_same_v<T, DataServiceError>)
          result += value.message.size();
        else
          result += value.digit.size();
      },
      event.value);
  return result;
}
}  // namespace

std::shared_ptr<RoomServices> RoomServiceState::handle() {
  return std::shared_ptr<RoomServices>(new RoomServices(shared_from_this()));
}

void RoomServiceState::configure(EncryptionOptions options,
                                 std::chrono::milliseconds operation_timeout) {
  std::lock_guard lock(mutex);
  if (active || closed)
    throw std::logic_error("Cannot configure an active/closed room service");
  encryption = std::move(options);
  timeout = operation_timeout;
  tracks->configure(encryption);
}

void RoomServiceState::attach_wakeup(std::shared_ptr<livekit::sdk::detail::WakeEvent> wakeup) {
  if (!wakeup) {
    throw std::invalid_argument("Room services require a wake source");
  }
  {
    std::lock_guard lock(mutex);
    if (active || closed || !outgoing.empty()) {
      throw std::logic_error("Attach room service wake source before connecting");
    }
    cancellations = std::make_shared<livekit::sdk::detail::ReadySet<uint64_t>>(wakeup, max_packets);
    work_wakeup = wakeup;
  }
  rpc.attach_wakeup(std::move(wakeup));
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
      pending.promise->set_exception(failure(ErrorCode::cancelled, "Data send cancelled"));
    } else if (now >= pending.deadline) {
      pending.promise->set_exception(failure(ErrorCode::timeout, "Data send timed out"));
    } else {
      continue;
    }
    erase_send_locked(found);
  }
}

void RoomServiceState::connect() {
  {
    std::lock_guard lock(mutex);
    if (closed)
      throw std::logic_error("Room services closed");
    active = true;
  }
  rpc.connect();
  streams->connect();
  tracks->connect();
}

void RoomServiceState::interrupt() {
  {
    std::lock_guard lock(mutex);
    if (!active)
      return;
    active = false;
    for (auto& [id, pending] : outgoing)
      pending.promise->set_exception(failure(ErrorCode::connection_lost, "Data send interrupted"));
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
    if (closed)
      return;
    closed = true;
    active = false;
    retired = std::move(handler);
    for (auto& [id, pending] : outgoing)
      pending.promise->set_exception(failure(ErrorCode::session_closed, "Room services closed"));
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

void RoomServiceState::receive_signal(const livekit::SignalResponse& response) {
  tracks->receive_signal(response);
}

std::future<void> RoomServiceState::enqueue(livekit::DataPacket packet, DataSendOptions options) {
  auto promise = std::make_shared<std::promise<void>>();
  auto result = promise->get_future();
  try {
    if (options.operation.wait_for_ack || options.operation.timeout.count() < 0 ||
        options.operation.timeout > std::chrono::minutes(5))
      throw std::invalid_argument("Invalid data send operation");
    if (options.destinations.size() > 128)
      throw std::invalid_argument("Too many data destinations");
    for (const auto& identity : options.destinations) {
      if (identity.empty() || identity.size() > 1024 || !livekit::sdk::detail::valid_utf8(identity))
        throw std::invalid_argument("Invalid data destination");
      packet.add_destination_identities(identity);
    }
    packet.set_kind(options.reliable ? 0 : 1);
    const auto bytes = packet.ByteSizeLong();
    // Leave room for the authenticated encryption envelope and server identity.
    if (bytes > 15000)
      throw std::invalid_argument("Data packet exceeds 15000 byte limit");
    std::lock_guard lock(mutex);
    if (!active || closed)
      std::rethrow_exception(failure(ErrorCode::session_closed, "Room services are not connected"));
    if (options.operation.stop.stop_requested())
      std::rethrow_exception(failure(ErrorCode::cancelled, "Data send cancelled"));
    if (outgoing.size() >= max_packets || bytes > max_bytes - outgoing_bytes)
      std::rethrow_exception(failure(ErrorCode::resource_exhausted, "Data send queue exhausted"));
    const auto id = ++next_send;
    const auto deadline =
        Clock::now() + (options.operation.timeout.count() ? options.operation.timeout : timeout);
    try {
      const auto wake = cancellations->watch(id);
      auto cancellation = std::make_shared<std::stop_callback<std::function<void()>>>(
          options.operation.stop, [wake] { wake->signal(); });
      send_deadlines.set(id, deadline);
      outgoing.emplace(id, Pending{std::move(packet), bytes, promise, deadline,
                                   options.operation.stop, std::move(cancellation)});
      outgoing_bytes += bytes;
      work_wakeup->signal();
    } catch (...) {
      send_deadlines.erase(id);
      cancellations->forget(id);
      throw;
    }
  } catch (...) {
    promise->set_exception(std::current_exception());
  }
  return result;
}

void RoomServiceState::emit(DataEvent event) {
  const auto bytes = size(event);
  std::lock_guard lock(mutex);
  if (!active || closed)
    return;
  if (incoming.size() >= max_packets || bytes > max_bytes - incoming_bytes) {
    if (!event.reliable) {
      ++dropped_events;
      return;
    }
    const auto error = SDKError(ErrorDomain::data, ErrorCode::resource_exhausted,
                                "Reliable data receive queue exhausted");
    callback_failure = error.failure();
    throw error;
  }
  incoming.push_back(std::move(event));
  incoming_bytes += bytes;
  schedule_events_locked();
}

void RoomServiceState::set_dispatcher(Dispatch value,
                                      std::function<void(std::exception_ptr)> failure) {
  std::lock_guard lock(mutex);
  if (active || closed || dispatch) {
    throw std::logic_error("Install room service notifications once, before connect");
  }
  dispatch = std::move(value);
  report_failure = std::move(failure);
}

void RoomServiceState::set_event_handler(std::function<void(const DataEvent&)> value) {
  auto next = value ? std::make_shared<Handler>() : nullptr;
  if (next) {
    next->call = std::move(value);
  }
  std::unique_lock lock(mutex);
  if (next && (!dispatch || closed || callback_failure)) {
    throw SDKError(ErrorDomain::lifecycle, ErrorCode::session_closed,
                   "Room service notification delivery is unavailable");
  }
  auto previous = std::exchange(handler, std::move(next));
  auto executing = in_flight;
  if (!handler) {
    incoming.clear();
    incoming_bytes = 0;
  }
  schedule_events_locked();
  if (executing && executing->thread != std::this_thread::get_id()) {
    changed.wait(lock, [&] { return !executing->running; });
  }
  lock.unlock();
}

RoomServiceSnapshot RoomServiceState::snapshot() const {
  std::lock_guard lock(mutex);
  return {
      active,         closed,           bool(handler),   incoming.size(), incoming_bytes,
      dropped_events, callback_failure, outgoing.size(), outgoing_bytes,  send_deadlines.size()};
}

void RoomServiceState::schedule_events_locked() {
  if (notification_scheduled || !handler || incoming.empty() || !active || closed) {
    return;
  }
  notification_scheduled = true;
  try {
    dispatch([weak = weak_from_this()] {
      if (auto state = weak.lock()) {
        state->deliver_events();
      }
    });
  } catch (...) {
    notification_scheduled = false;
    throw;
  }
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
    std::exception_ptr error;
    std::shared_ptr<Handler> retired;
    try {
      target->call(*event);
    } catch (...) {
      error = normalized_failure(std::current_exception(), {"data_callback", {}, {}, 0},
                                 ErrorDomain::data, ErrorCode::callback_failed);
    }
    {
      std::lock_guard lock(mutex);
      target->running = false;
      target->thread = {};
      in_flight.reset();
      if (error) {
        callback_failure = describe_failure(error);
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
  std::lock_guard lock(mutex);
  notification_scheduled = false;
  schedule_events_locked();
}

void RoomServiceState::receive(const std::string& label, std::span<const uint8_t> bytes) {
  if (label == "_data_track") {
    tracks->receive_packet(bytes);
    return;
  }
  if (bytes.size() > 16384)
    return;
  livekit::DataPacket packet;
  if (!packet.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())))
    return;
  try {
    packet = livekit::sdk::detail::decrypt_packet(packet, encryption);
  } catch (const std::exception&) {
    emit({packet.participant_identity(), label == "_reliable",
          DataServiceError{"Data authentication failed",
                           describe_failure(std::current_exception(), {"decrypt_data", {}, {}, 0},
                                            ErrorDomain::data, ErrorCode::permission_denied)}});
    return;
  }
  if (packet.has_rpc_request() || packet.has_rpc_ack() || packet.has_rpc_response()) {
    rpc.receive(packet);
    return;
  }
  if (packet.has_stream_header() || packet.has_stream_chunk() || packet.has_stream_trailer()) {
    streams->receive(packet);
    return;
  }
  DataEvent event;
  event.sender_identity = packet.participant_identity();
  event.reliable = label == "_reliable";
  if (packet.has_user()) {
    if (event.sender_identity.empty())
      event.sender_identity = packet.user().participant_identity();
    const auto& data = packet.user().payload();
    event.value = ReceivedData{packet.user().topic(), {data.begin(), data.end()}};
  } else if (packet.has_sip_dtmf()) {
    DtmfDigit value{packet.sip_dtmf().code(), packet.sip_dtmf().digit()};
    try {
      livekit::sdk::detail::validate_dtmf(value);
    } catch (const std::invalid_argument&) {
      return;
    }
    event.value = std::move(value);
  } else if (packet.has_transcription()) {
    try {
      event.value = livekit::sdk::detail::parse_transcription(
          livekit::sdk::detail::transcription_json(packet.transcription()));
    } catch (const std::invalid_argument&) {
      return;
    }
  } else
    return;
  emit(std::move(event));
}

bool RoomServiceState::send_packet(const livekit::DataPacket& packet, const Send& send) {
  const auto wire = livekit::sdk::detail::encrypt_packet(packet, encryption).SerializeAsString();
  if (wire.size() > 16384)
    throw std::invalid_argument("Encrypted data packet exceeds channel limit");
  return send(packet.kind() == 0 ? "_reliable" : "_lossy",
              {reinterpret_cast<const uint8_t*>(wire.data()), wire.size()});
}

void RoomServiceState::pump(const Send& send,
                            const std::function<void(const livekit::SignalRequest&)>& signal,
                            const std::function<bool()>& reliable_ready) {
  {
    std::lock_guard lock(mutex);
    if (!active || closed)
      return;
    // Cancellation and expiry must progress behind a backpressured FIFO head.
    expire_sends_locked();
  }
  bool data_blocked = false;
  for (int i = 0; i < 4; ++i) {
    std::optional<Pending> pending;
    {
      std::lock_guard lock(mutex);
      if (outgoing.empty())
        break;
      pending = outgoing.begin()->second;
    }
    std::exception_ptr error;
    if (pending->stop.stop_requested())
      error = failure(ErrorCode::cancelled, "Data send cancelled");
    else if (Clock::now() >= pending->deadline)
      error = failure(ErrorCode::timeout, "Data send timed out");
    else
      try {
        if (!send_packet(pending->packet, send)) {
          data_blocked = true;
          break;
        }
      } catch (...) {
        error = std::current_exception();
      }
    std::lock_guard lock(mutex);
    if (outgoing.empty() || outgoing.begin()->second.promise != pending->promise)
      continue;
    if (error)
      pending->promise->set_exception(error);
    else
      pending->promise->set_value();
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
  if (rpc_pending.empty())
    for (auto& packet : rpc.take_packets(4))
      rpc_pending.push_back(std::move(packet));
  while (!rpc_pending.empty()) {
    if (!rpc.sendable(rpc_pending.front())) {
      rpc_pending.pop_front();
      continue;
    }
    try {
      if (!send_packet(rpc_pending.front(), send))
        break;
    } catch (...) {
      rpc.send_failed(rpc_pending.front());
    }
    rpc_pending.pop_front();
  }
  if (rpc_pending.empty() && rpc.has_outgoing()) {
    work_wakeup->signal();
  }
  streams->pump([&](const auto& packet) { return send_packet(packet, send); }, 4, reliable_ready);
  for (const auto& request : tracks->take_signals())
    signal(request);
  for (int i = 0; i < 4; ++i) {
    auto packet = tracks->take_packet();
    if (!packet)
      break;
    tracks->packet_sent(*packet, send("_data_track", *packet));
  }
}
}  // namespace livekit::sdk::rtc::detail

namespace livekit::sdk::rtc {
std::future<void> RoomServices::send_data(std::vector<uint8_t> bytes, DataSendOptions options) {
  if (options.topic.size() > 256 || !livekit::sdk::detail::valid_utf8(options.topic))
    throw std::invalid_argument("Invalid data topic");
  livekit::DataPacket packet;
  packet.mutable_user()->set_payload(bytes.data(), bytes.size());
  packet.mutable_user()->set_topic(options.topic);
  return state_->enqueue(std::move(packet), std::move(options));
}

std::future<void> RoomServices::send_dtmf(DtmfDigit value, DataSendOptions options) {
  return state_->enqueue(livekit::sdk::detail::encode_dtmf(value), std::move(options));
}

std::future<void> RoomServices::send_transcription(Transcription value, DataSendOptions options) {
  return state_->enqueue(livekit::sdk::detail::encode_transcription(value), std::move(options));
}

void RoomServices::set_event_handler(std::function<void(const DataEvent&)> handler) {
  state_->set_event_handler(std::move(handler));
}

RoomServiceSnapshot RoomServices::snapshot() const {
  return state_->snapshot();
}

void RoomServices::register_rpc(std::string method, RpcHandler handler) {
  state_->rpc.register_method(method, std::move(handler));
}

void RoomServices::unregister_rpc(const std::string& method) {
  state_->rpc.unregister_method(method);
}

std::future<std::string> RoomServices::call_rpc(RpcCallOptions options) {
  return state_->rpc.call(options);
}

void RoomServices::register_stream(bool text, std::string topic, StreamHandler handler) {
  state_->streams->register_handler(text, topic, std::move(handler));
}

void RoomServices::unregister_stream(bool text, const std::string& topic) {
  state_->streams->unregister_handler(text, topic);
}

std::shared_ptr<StreamWriter> RoomServices::open_stream(bool text, StreamOptions options) {
  return state_->streams->open(text, std::move(options));
}

std::future<StreamInfo> RoomServices::send_text(std::string value, StreamOptions options,
                                                OperationOptions operation) {
  return state_->streams->send_text(std::move(value), std::move(options), operation);
}

std::future<StreamInfo> RoomServices::send_file(std::filesystem::path path, StreamOptions options,
                                                OperationOptions operation) {
  return state_->streams->send_file(std::move(path), std::move(options), operation);
}

std::future<LocalDataTrack> RoomServices::publish_data_track(std::string name,
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
