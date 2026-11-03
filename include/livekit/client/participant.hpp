#pragma once

#include "livekit/client/track_publication.hpp"
#include "livekit/client/rpc.hpp"
#include "livekit/client/streams.hpp"
#include "livekit/client/data_track.hpp"
#include "livekit/data_messages.hpp"

namespace livekit::sdk::client {
namespace detail {
struct ParticipantData;
}  // namespace detail

class LocalParticipant {
 public:
  LocalParticipant() = default;

  explicit operator bool() const {
    return bool(data_);
  }

  ParticipantSnapshot snapshot() const;
  std::vector<LocalTrackPublication> track_publications() const;
  Result<LocalTrackPublication> publish_track(LocalTrack track, TrackPublishOptions options = {});
  void unpublish_track(TrackSid sid);
  void publish_data(DataPacket packet);
  void update(ParticipantUpdate update);

  // Handlers run on bounded service workers; never on RoomObserver/transport threads.
  // They must honor invocation cancellation/reader closure before Room destruction.
  void register_rpc(std::string method, RpcHandler handler);
  void unregister_rpc(const std::string& method);
  Result<std::string> perform_rpc(RpcCallOptions options);
  void register_stream(bool text, std::string topic, StreamHandler handler);
  void unregister_stream(bool text, const std::string& topic);
  Result<std::shared_ptr<StreamWriter>> open_stream(bool text, StreamOptions options = {});
  Result<StreamInfo> send_text(std::string text, StreamOptions options = {},
                               OperationOptions operation = {});
  Result<StreamInfo> send_file(std::filesystem::path path, StreamOptions options = {},
                               OperationOptions operation = {});
  Result<LocalDataTrack> publish_data_track(std::string name, DataTrackOptions options = {},
                                            OperationOptions operation = {});
  std::vector<LocalDataTrack> data_tracks() const;
  void publish_dtmf(DtmfDigit, std::vector<std::string> destinations = {});
  void publish_transcription(Transcription, std::vector<std::string> destinations = {});

 private:
  friend class detail::RoomSession;

  explicit LocalParticipant(std::shared_ptr<detail::ParticipantData> data)
      : data_(std::move(data)) {}

  std::shared_ptr<detail::ParticipantData> data_;
};

class RemoteParticipant {
 public:
  RemoteParticipant() = default;

  explicit operator bool() const {
    return bool(data_);
  }

  ParticipantSnapshot snapshot() const;
  std::vector<RemoteTrackPublication> track_publications() const;
  std::vector<RemoteDataTrack> data_tracks() const;

 private:
  friend class detail::RoomSession;

  explicit RemoteParticipant(std::shared_ptr<detail::ParticipantData> data)
      : data_(std::move(data)) {}

  std::shared_ptr<detail::ParticipantData> data_;
};
}  // namespace livekit::sdk::client
