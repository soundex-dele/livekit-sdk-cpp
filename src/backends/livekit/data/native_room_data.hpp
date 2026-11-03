#pragma once
#include "backends/livekit/room_data.hpp"
#include "backends/livekit/data/rpc_session.hpp"
#include "backends/livekit/data/stream_session.hpp"
#include "backends/livekit/data/data_track_session.hpp"

namespace livekit::sdk::client::detail {
class NativeRoomData final : public RoomData {
 public:
  ~NativeRoomData() override;
  Result<void> connect(const EncryptionOptions&);
  void interrupt(bool rejoin) override;
  void shutdown() override;
  void participant(const livekit::ParticipantInfo&);
  Result<void> signal(const livekit::SignalResponse&);
  void receive(const livekit::DataPacket&);
  Result<void> receive_track(livekit::compat::span<const uint8_t>);
  Result<void> pump(const std::function<Result<bool>(const livekit::DataPacket&)>&,
                    const std::function<Result<void>(const livekit::SignalRequest&)>&,
                    const std::function<bool(const std::vector<uint8_t>&)>&);

  Result<void> register_rpc(std::string method, RpcHandler handler) override;
  void unregister_rpc(const std::string&) override;
  Result<std::string> perform_rpc(RpcCallOptions) override;
  Result<void> register_stream(bool, std::string, StreamHandler) override;
  void unregister_stream(bool, const std::string&) override;
  Result<std::shared_ptr<StreamWriter>> open_stream(bool, StreamOptions) override;
  Result<StreamInfo> send_text(std::string, StreamOptions, OperationOptions) override;
  Result<StreamInfo> send_file(std::filesystem::path, StreamOptions, OperationOptions) override;
  Result<LocalDataTrack> publish_data_track(std::string, DataTrackOptions,
                                            OperationOptions) override;
  std::vector<LocalDataTrack> local_data_tracks() override;
  std::vector<RemoteDataTrack> remote_data_tracks() override;

 private:
  RpcSession rpc_;
  std::shared_ptr<StreamSession> streams_ = std::make_shared<StreamSession>();
  std::shared_ptr<DataTrackSession> tracks_ = std::make_shared<DataTrackSession>();
  bool configured_ = false, active_ = false;
};
}  // namespace livekit::sdk::client::detail
