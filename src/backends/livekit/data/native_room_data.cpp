#include "backends/livekit/data/native_room_data.hpp"

namespace livekit::sdk::client::detail {
std::shared_ptr<RoomData> create_room_data() {
  return std::make_shared<NativeRoomData>();
}

NativeRoomData::~NativeRoomData() {
  shutdown();
}

void NativeRoomData::shutdown() {
  interrupt(false);
  rpc_.shutdown();
  streams_->shutdown();
}

Result<void> NativeRoomData::connect(const EncryptionOptions& options) {
  if (!configured_) {
    auto result = tracks_->configure(options);
    if (!result) {
      return result;
    }
    configured_ = true;
  }
  rpc_.connect();
  streams_->connect();
  tracks_->connect();
  active_ = true;
  return Result<void>::success();
}

void NativeRoomData::interrupt(bool rejoin) {
  rpc_.disconnect();
  streams_->disconnect();
  if (rejoin) {
    tracks_->rejoin();
  } else {
    tracks_->disconnect();
  }
  active_ = false;
}

void NativeRoomData::participant(const livekit::ParticipantInfo& participant) {
  tracks_->participant(participant);
  if (participant.state() == 3) {
    rpc_.participant_left(participant.identity());
    streams_->participant_left(participant.identity());
  }
}

Result<void> NativeRoomData::signal(const livekit::SignalResponse& response) {
  return tracks_->receive_signal(response);
}

void NativeRoomData::receive(const livekit::DataPacket& packet) {
  rpc_.receive(packet);
  streams_->receive(packet);
}

Result<void> NativeRoomData::receive_track(livekit::compat::span<const uint8_t> packet) {
  return tracks_->receive_packet(packet);
}

Result<void> NativeRoomData::pump(
    const std::function<Result<bool>(const livekit::DataPacket&)>& send,
    const std::function<Result<void>(const livekit::SignalRequest&)>& signal,
    const std::function<bool(const std::vector<uint8_t>&)>& send_track) {
  if (!active_) {
    return Result<void>::success();
  }
  for (const auto& packet : rpc_.take_packets()) {
    if (!rpc_.sendable(packet)) {
      continue;
    }
    auto result = send(packet);
    if (!result || !*result) {
      rpc_.send_failed(packet);
    }
  }
  streams_->pump(send);
  auto signals = tracks_->take_signals();
  if (!signals) {
    return Result<void>::failure(signals.failure_record());
  }
  for (const auto& message : *signals) {
    if (auto result = signal(message); !result) {
      return result;
    }
  }
  for (int i = 0; i < 16; ++i) {
    auto packet = tracks_->take_packet();
    if (!packet) {
      return Result<void>::failure(packet.failure_record());
    }
    if (!*packet) {
      break;
    }
    const bool sent = send_track(**packet);
    tracks_->packet_sent(**packet, sent);
    if (!sent) {
      break;
    }
  }
  return Result<void>::success();
}

Result<void> NativeRoomData::register_rpc(std::string method, RpcHandler handler) {
  return rpc_.register_method(method, std::move(handler));
}

void NativeRoomData::unregister_rpc(const std::string& method) {
  rpc_.unregister_method(method);
}

Result<std::string> NativeRoomData::perform_rpc(RpcCallOptions options) {
  return rpc_.call(options).get();
}

Result<void> NativeRoomData::register_stream(bool text, std::string topic, StreamHandler handler) {
  return streams_->register_handler(text, topic, std::move(handler));
}

void NativeRoomData::unregister_stream(bool text, const std::string& topic) {
  streams_->unregister_handler(text, topic);
}

Result<std::shared_ptr<StreamWriter>> NativeRoomData::open_stream(bool text,
                                                                  StreamOptions options) {
  return streams_->open(text, std::move(options));
}

Result<StreamInfo> NativeRoomData::send_text(std::string text, StreamOptions options,
                                             OperationOptions operation) {
  return streams_->send_text(std::move(text), std::move(options), operation).get();
}

Result<StreamInfo> NativeRoomData::send_file(std::filesystem::path path, StreamOptions options,
                                             OperationOptions operation) {
  return streams_->send_file(std::move(path), std::move(options), operation).get();
}

Result<LocalDataTrack> NativeRoomData::publish_data_track(std::string name,
                                                          DataTrackOptions options,
                                                          OperationOptions operation) {
  return tracks_->publish(std::move(name), std::move(options), operation).get();
}

std::vector<LocalDataTrack> NativeRoomData::local_data_tracks() {
  return tracks_->local_tracks();
}

std::vector<RemoteDataTrack> NativeRoomData::remote_data_tracks() {
  return tracks_->remote_tracks();
}
}  // namespace livekit::sdk::client::detail
