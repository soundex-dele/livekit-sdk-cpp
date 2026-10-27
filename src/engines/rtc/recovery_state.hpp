#pragma once
#include "livekit/room.hpp"
#include "engines/rtc/reconnect_policy.hpp"
#include "engines/rtc/region_policy.hpp"
#include <map>
#include <optional>

namespace livekit::sdk::detail {
struct DesiredPublication {
  MediaSpec media;
  TrackPublishOptions options;
  bool microphone = false;
  bool muted = false;
};
struct RemoteIntent {
  std::string identity, sid, name;
  TrackKind kind = TrackKind::audio;
  TrackSource source = TrackSource::unknown;
  std::optional<bool> subscribed;
  Json settings = Json::object();
};
// This state is owned solely by the Room runner, survives destruction of one
// native transport, and never retains WebRTC pointers or completed commands.
struct RecoveryState {
  RecoveryBudget budget;
  bool ever_connected = false, rejoining = false, initial_data_sent = false;
  uint64_t next_source = 0;
  std::string token;
  std::string expected_room;
  bool room_changed = false;
  RegionState regions;
  std::optional<SubscriptionPermissions> subscription_permissions;
  std::optional<ParticipantInfo> local_snapshot;
  RecoveryBudget::Clock::time_point session_deadline = RecoveryBudget::Clock::time_point::max();
  std::map<std::string, DesiredPublication> publications;
  std::map<std::string, RemoteIntent> remote;
  std::map<std::string, std::string> local_ids; // current server SID -> stable source ID
  std::string allocate_id(bool video) {
    return std::string(video ? "video-" : "audio-") + std::to_string(++next_source);
  }
  void validate_sources() const {
    for (const auto& [id, p] : publications) {
      const auto& m = p.media;
      if (!p.microphone && (m.reconnect == MediaRestartPolicy::fail ||
          (m.reconnect == MediaRestartPolicy::automatic && !m.demo && !m.loop && !m.video_source && !m.audio_source && m.capture == CaptureKind::input)))
        throw SDKError(ErrorDomain::media, ErrorCode::unsupported,
                       "Source cannot be restarted automatically after rejoin: " + id);
    }
  }
};
}
