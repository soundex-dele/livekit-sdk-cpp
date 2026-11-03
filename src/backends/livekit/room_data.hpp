#pragma once
#include "livekit/client/rpc.hpp"
#include "livekit/client/streams.hpp"
#include "livekit/client/data_track.hpp"
#include "livekit/client/encryption.hpp"

namespace livekit::sdk::client::detail {
// Service handlers may use blocking data APIs, but must not join their own room.
inline thread_local bool service_handler_thread = false;

// Participant-owned services survive transport replacement. Blocking calls run
// on application threads; the RtcSession owner drains their bounded queues.
class RoomData {
 public:
  virtual ~RoomData() = default;
  virtual void interrupt(bool rejoin) = 0;
  virtual void shutdown() = 0;
  virtual Result<void> register_rpc(std::string, RpcHandler) = 0;
  virtual void unregister_rpc(const std::string&) = 0;
  virtual Result<std::string> perform_rpc(RpcCallOptions) = 0;
  virtual Result<void> register_stream(bool, std::string, StreamHandler) = 0;
  virtual void unregister_stream(bool, const std::string&) = 0;
  virtual Result<std::shared_ptr<StreamWriter>> open_stream(bool, StreamOptions) = 0;
  virtual Result<StreamInfo> send_text(std::string, StreamOptions, OperationOptions) = 0;
  virtual Result<StreamInfo> send_file(std::filesystem::path, StreamOptions, OperationOptions) = 0;
  virtual Result<LocalDataTrack> publish_data_track(std::string, DataTrackOptions,
                                                    OperationOptions) = 0;
  virtual std::vector<LocalDataTrack> local_data_tracks() = 0;
  virtual std::vector<RemoteDataTrack> remote_data_tracks() = 0;
};

std::shared_ptr<RoomData> create_room_data();
}  // namespace livekit::sdk::client::detail
