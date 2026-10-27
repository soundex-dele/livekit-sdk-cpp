#pragma once
#include "livekit/auth.hpp"
#include "livekit/media.hpp"
#include "livekit/operation.hpp"
#include <future>
#include <map>
#include <memory>
#include <vector>

namespace livekit::sdk {
namespace detail { struct RoomState; }
enum class TrackKind { audio = 0, video = 1, data = 2 };
enum class TrackSource { unknown = 0, camera = 1, microphone = 2, screen_share = 3, screen_share_audio = 4 };
struct TrackInfo {
  std::string sid, name, mime_type;
  TrackKind kind = TrackKind::audio;
  TrackSource source = TrackSource::unknown;
  bool muted = false, subscribed = false;
  int width = 0, height = 0;
  // Stable SDK source ID for local publications, including across full rejoin.
  // Remote publications may leave this empty. sid remains the server ID.
  std::string client_id;
};
struct ParticipantInfo {
  std::string sid, identity, name, metadata;
  std::map<std::string, std::string> attributes;
  std::vector<TrackInfo> tracks;
  int state = 0, kind = 0, disconnect_reason = 0;
  bool speaking = false;
  float audio_level = 0;
};
struct TrackPublishOptions {
  std::string name, stream;
  TrackSource source = TrackSource::unknown;
};
struct DataPublishOptions {
  bool reliable = false; // matches Go: lossy unless explicitly enabled
  std::string topic;
  std::vector<std::string> destination_identities;
};
struct DataReceiveParams {
  std::string sender_identity, topic;
  bool reliable = false;
};

class TrackPublication {
 public:
  TrackInfo info() const;
  std::string sid() const { return snapshot_.sid; }
  std::future<void> set_muted(bool muted, const OperationOptions& operation = {}) const; // local publication
  std::future<void> set_subscribed(bool subscribed, const OperationOptions& operation = {}) const; // remote publication
  std::future<void> set_enabled(bool enabled, const OperationOptions& operation = {}) const; // remote stream settings
  std::future<void> set_video_dimensions(int width, int height, const OperationOptions& operation = {}) const;
  // Handles are normally obtained from participants/callbacks, not constructed by applications.
  TrackPublication(std::weak_ptr<detail::RoomState>, uint64_t generation, std::string owner, TrackInfo snapshot, bool local);
 private:
  std::weak_ptr<detail::RoomState> state_;
  uint64_t generation_;
  std::string owner_;
  TrackInfo snapshot_;
  bool local_;
};
class Participant {
 public:
  ParticipantInfo info() const;
  std::string identity() const { return snapshot_.identity; }
  std::string sid() const { return snapshot_.sid; }
  std::vector<TrackPublication> track_publications() const;
  Participant(std::weak_ptr<detail::RoomState>, uint64_t generation, ParticipantInfo snapshot, bool local);
 protected:
  std::weak_ptr<detail::RoomState> state_;
  uint64_t generation_;
  ParticipantInfo snapshot_;
  bool local_;
};
class RemoteParticipant : public Participant { public: using Participant::Participant; };
class LocalParticipant : public Participant {
 public:
  using Participant::Participant;
  std::future<TrackPublication> publish_track(const MediaSpec& media, const TrackPublishOptions& options = {}, const OperationOptions& operation = {}) const;
  std::future<void> unpublish_track(const std::string& sid, const OperationOptions& operation = {}) const;
  std::future<void> publish_data(const std::vector<uint8_t>& payload, const DataPublishOptions& options = {}, const OperationOptions& operation = {}) const;
  std::future<void> set_name(const std::string& name, const OperationOptions& operation = {}) const;
  std::future<void> set_metadata(const std::string& metadata, const OperationOptions& operation = {}) const;
  std::future<void> set_attributes(const std::map<std::string, std::string>& attributes, const OperationOptions& operation = {}) const;
};
}
