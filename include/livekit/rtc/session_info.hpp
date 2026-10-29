#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace livekit::sdk::rtc {
enum class PublicationKind { audio = 0, video = 1, data = 2 };
enum class PublicationSource {
  unknown = 0,
  camera = 1,
  microphone = 2,
  screen = 3,
  screen_audio = 4
};

struct PublicationInfo {
  std::string sid, name, mime_type, stream;
  PublicationKind kind = PublicationKind::audio;
  PublicationSource source = PublicationSource::unknown;
  bool muted = false;
  uint32_t width = 0, height = 0;
};

struct ParticipantPermissions {
  bool can_subscribe = false, can_publish = false, can_publish_data = false;
  bool can_update_metadata = false, hidden = false;
  std::vector<PublicationSource> can_publish_sources;
};

struct ParticipantSnapshot {
  std::string sid, identity, name, metadata;
  std::map<std::string, std::string> attributes;
  std::map<std::string, PublicationInfo> publications;  // Server SID, not local TrackId.
  std::optional<ParticipantPermissions> permissions;    // Absent is unknown, not a denial.
  uint32_t version = 0;
  int state = 0, kind = 0, disconnect_reason = 0;
  bool speaking = false;
  float audio_level = 0;
  std::optional<int> connection_quality;
};

struct RoomInfo {
  std::string sid, name, metadata;
  bool recording = false;
};

struct ParticipantUpdate {
  // Wire semantics: empty name/metadata means unchanged. Attribute values of
  // "" delete those keys; omitted keys remain unchanged.
  std::string name, metadata;
  std::map<std::string, std::string> attributes;
};

// Immutable observations of the current signaling session. Advertised tracks
// can include unsupported codecs and do not imply a subscription/decoder exists.
struct SessionInfo {
  uint64_t epoch = 0, revision = 0;
  RoomInfo room;
  ParticipantSnapshot local;
  std::map<std::string, ParticipantSnapshot> remote;  // Participant SID.
};
}  // namespace livekit::sdk::rtc
