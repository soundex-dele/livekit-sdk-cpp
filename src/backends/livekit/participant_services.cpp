#include "backends/livekit/participant_state.hpp"
#include "backends/livekit/room_data.hpp"
#include "support/logger.h"

namespace livekit::sdk::client {
namespace {
Result<std::shared_ptr<detail::RoomData>> services(
    const std::shared_ptr<detail::ParticipantData>& p) {
  using Return = Result<std::shared_ptr<detail::RoomData>>;
  if (detail::sdk_thread) {
    return Return::failure(make_failure(ErrorDomain::lifecycle, ErrorCode::invalid_thread,
                                        "Data service operation on SDK callback thread"));
  }
  auto engine = p ? p->engine.lock() : nullptr;
  if (!engine || !engine->available()) {
    return Return::failure(
        make_failure(ErrorDomain::lifecycle, ErrorCode::session_closed, "Room is not connected"));
  }
  auto value = engine->data_services();
  if (!value) {
    return Return::failure(make_failure(ErrorDomain::data, ErrorCode::unsupported,
                                        "Native data services are not built"));
  }
  return Return::success(std::move(value));
}
}  // namespace

void LocalParticipant::register_rpc(std::string method, RpcHandler handler) {
  auto service = services(data_);
  if (!service) {
    LOG_ERROR("LiveKitClient", "register_rpc failed: " << service.error().message);
    return;
  }
  auto result = (*service)->register_rpc(std::move(method), std::move(handler));
  if (!result) {
    LOG_ERROR("LiveKitClient", "register_rpc failed: " << result.error().message);
  }
}

void LocalParticipant::unregister_rpc(const std::string& method) {
  auto service = services(data_);
  if (!service) {
    LOG_ERROR("LiveKitClient", "unregister_rpc failed: " << service.error().message);
    return;
  }
  (*service)->unregister_rpc(method);
}

void LocalParticipant::register_stream(bool text, std::string topic, StreamHandler handler) {
  auto service = services(data_);
  if (!service) {
    LOG_ERROR("LiveKitClient", "register_stream failed: " << service.error().message);
    return;
  }
  auto result = (*service)->register_stream(text, std::move(topic), std::move(handler));
  if (!result) {
    LOG_ERROR("LiveKitClient", "register_stream failed: " << result.error().message);
  }
}

void LocalParticipant::unregister_stream(bool text, const std::string& topic) {
  auto service = services(data_);
  if (!service) {
    LOG_ERROR("LiveKitClient", "unregister_stream failed: " << service.error().message);
    return;
  }
  (*service)->unregister_stream(text, topic);
}

Result<std::string> LocalParticipant::perform_rpc(RpcCallOptions options) {
  auto service = services(data_);
  if (!service) {
    return Result<std::string>::failure(service.failure_record());
  }
  return (*service)->perform_rpc(std::move(options));
}

Result<std::shared_ptr<StreamWriter>> LocalParticipant::open_stream(bool text,
                                                                    StreamOptions options) {
  auto service = services(data_);
  if (!service) {
    return Result<std::shared_ptr<StreamWriter>>::failure(service.failure_record());
  }
  return (*service)->open_stream(text, std::move(options));
}

Result<StreamInfo> LocalParticipant::send_text(std::string text, StreamOptions options,
                                               OperationOptions operation) {
  auto service = services(data_);
  if (!service) {
    return Result<StreamInfo>::failure(service.failure_record());
  }
  return (*service)->send_text(std::move(text), std::move(options), operation);
}

Result<StreamInfo> LocalParticipant::send_file(std::filesystem::path path, StreamOptions options,
                                               OperationOptions operation) {
  auto service = services(data_);
  if (!service) {
    return Result<StreamInfo>::failure(service.failure_record());
  }
  return (*service)->send_file(std::move(path), std::move(options), operation);
}

Result<LocalDataTrack> LocalParticipant::publish_data_track(std::string name,
                                                            DataTrackOptions options,
                                                            OperationOptions operation) {
  auto service = services(data_);
  if (!service) {
    return Result<LocalDataTrack>::failure(service.failure_record());
  }
  return (*service)->publish_data_track(std::move(name), std::move(options), operation);
}

std::vector<LocalDataTrack> LocalParticipant::data_tracks() const {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  auto service = engine ? engine->data_services() : nullptr;
  return service ? service->local_data_tracks() : std::vector<LocalDataTrack>{};
}

std::vector<RemoteDataTrack> RemoteParticipant::data_tracks() const {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  auto service = engine ? engine->data_services() : nullptr;
  std::vector<RemoteDataTrack> tracks;
  if (service) {
    const auto identity = snapshot().identity;
    for (auto& track : service->remote_data_tracks()) {
      if (track.info().publisher_identity == identity) {
        tracks.push_back(std::move(track));
      }
    }
  }
  return tracks;
}

void LocalParticipant::publish_dtmf(DtmfDigit value, std::vector<std::string> destinations) {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    LOG_ERROR("LiveKitClient", "publish_dtmf: Room destroyed");
    return;
  }
  auto result = engine->perform([value = std::move(value), destinations = std::move(destinations)](
                                    detail::RtcSession& s, const detail::Operation& op) {
    return s.publish_dtmf(value, destinations, op);
  });
  if (!result) {
    LOG_ERROR("LiveKitClient", "publish_dtmf failed: " << result.error().message);
  }
}

void LocalParticipant::publish_transcription(Transcription value,
                                             std::vector<std::string> destinations) {
  auto engine = data_ ? data_->engine.lock() : nullptr;
  if (!engine) {
    LOG_ERROR("LiveKitClient", "publish_transcription: Room destroyed");
    return;
  }
  auto result = engine->perform([value = std::move(value), destinations = std::move(destinations)](
                                    detail::RtcSession& s, const detail::Operation& op) {
    return s.publish_transcription(value, destinations, op);
  });
  if (!result) {
    LOG_ERROR("LiveKitClient", "publish_transcription failed: " << result.error().message);
  }
}
}  // namespace livekit::sdk::client
