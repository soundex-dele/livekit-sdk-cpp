#pragma once
#include "livekit/data_messages.hpp"
#include "livekit/data_track.hpp"
#include "livekit/rpc.hpp"
#include "livekit/streams.hpp"
#include <variant>

namespace livekit::sdk::rtc {
namespace detail {
struct RoomServiceState;
}

struct DataSendOptions {
  bool reliable = true;
  std::string topic;
  std::vector<std::string> destinations;
  OperationOptions operation;
};

struct ReceivedData {
  std::string topic;
  std::vector<uint8_t> bytes;
};

struct DataServiceError {
  std::string message;
};

struct DataEvent {
  std::string sender_identity;
  bool reliable = true;
  std::variant<ReceivedData, DtmfDigit, Transcription, DataServiceError> value;
};

// Session services share the room's transport and lifetime. This handle can be
// retained after RoomSession destruction; active operations are then rejected.
// Network callbacks only enqueue data. RPC/stream handlers run on bounded
// service executors, never on the native transport/signaling thread.
class RoomServices {
 public:
  std::future<void> send_data(std::vector<uint8_t>, DataSendOptions = {});
  std::future<void> send_dtmf(DtmfDigit, DataSendOptions = {});
  std::future<void> send_transcription(Transcription, DataSendOptions = {});
  std::optional<DataEvent> next_event(std::chrono::milliseconds timeout, std::stop_token stop = {});

  void register_rpc(std::string method, RpcHandler);
  void unregister_rpc(const std::string& method);
  std::future<std::string> call_rpc(RpcCallOptions);

  void register_stream(bool text, std::string topic, StreamHandler);
  void unregister_stream(bool text, const std::string& topic);
  std::shared_ptr<StreamWriter> open_stream(bool text, StreamOptions = {});
  std::future<StreamInfo> send_text(std::string, StreamOptions = {}, OperationOptions = {});
  std::future<StreamInfo> send_file(std::filesystem::path, StreamOptions = {},
                                    OperationOptions = {});

  std::future<LocalDataTrack> publish_data_track(std::string name, DataTrackOptions = {},
                                                 OperationOptions = {});
  std::vector<LocalDataTrack> local_data_tracks() const;
  std::vector<RemoteDataTrack> remote_data_tracks() const;

 private:
  friend struct detail::RoomServiceState;

  explicit RoomServices(std::shared_ptr<detail::RoomServiceState> state)
      : state_(std::move(state)) {}

  std::shared_ptr<detail::RoomServiceState> state_;
};
}  // namespace livekit::sdk::rtc
