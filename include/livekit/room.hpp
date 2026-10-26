#pragma once
#include "livekit/auth.hpp"
#include "livekit/media.hpp"
#include "livekit/participant.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <optional>

namespace livekit::sdk {
using EventHandler = std::function<void(const Json&)>;
enum class ConnectionState { disconnected, connecting, connected };
struct RoomInfo { std::string sid, name, metadata; bool recording = false; };
struct ConnectInfo {
  std::string api_key, api_secret, room_name, participant_identity, participant_name, participant_metadata;
  std::map<std::string, std::string> participant_attributes;
};
struct RoomCallback {
  std::function<void()> on_connected;
  std::function<void(int reason)> on_disconnected;
  std::function<void(const RemoteParticipant&)> on_participant_connected, on_participant_disconnected;
  std::function<void(const TrackPublication&, const RemoteParticipant&)> on_track_published, on_track_unpublished, on_track_subscribed, on_track_unsubscribed;
  std::function<void(const TrackPublication&, const LocalParticipant&)> on_local_track_published, on_local_track_unpublished;
  std::function<void(const TrackPublication&, const Participant&)> on_track_muted, on_track_unmuted;
  std::function<void(const std::string& old_metadata, const Participant&)> on_metadata_changed;
  std::function<void(const std::map<std::string, std::string>& changes, const Participant&)> on_attributes_changed;
  std::function<void(const std::vector<uint8_t>&, const DataReceiveParams&)> on_data_received;
  std::function<void(const std::string&)> on_room_metadata_changed;
  std::function<void(bool)> on_recording_status_changed;
  std::function<void(const std::vector<Participant>&)> on_active_speakers_changed;
  std::function<void(const std::string& sid, int quality)> on_connection_quality_changed;
  std::function<void(const std::string& sid, int error)> on_track_subscription_failed;
};
struct RoomOptions {
  std::string url, token, room, identity;
  std::vector<MediaSpec> media;
  std::filesystem::path record;
  bool subscribe = true, audio_playout = false, video_playout = false;
  bool microphone = false, exit_after_publish = false;
  std::vector<std::string> subscribe_tracks;
  std::string publish_data, topic, codec = "vp8";
  int duration = 0, connect_timeout = 30;
  bool relay_only = false, disable_turn = false;
  // Optional native W3C media/transport stats emitted as "rtc-stats" events.
  // 0 disables collection; otherwise 200..60000 ms. Callbacks stay serialized.
  int stats_interval_ms = 0;
  // Latest remote frame per track, serialized with events on the run thread.
  // Copy the frame to retain it. Exceptions terminate and clean up the session.
  VideoFrameHandler video_frame_handler;
};

class Room {
 public:
  explicit Room(RoomCallback callbacks = {});
  ~Room();
  Room(const Room&) = delete;
  Room& operator=(const Room&) = delete;
  // Blocking session; callbacks are serialized on this calling thread.
  // Use the main thread when video_playout is enabled. The object must outlive run().
  // Callback exceptions are propagated after native resources have been cleaned up.
  void run(const RoomOptions& options, EventHandler handler = {}, std::stop_token stop = {});
  // Thread-safe, also callable from an event handler. Does not join the run thread.
  void request_stop() noexcept;
  // Returns once connected; native processing/callbacks then run on an owned worker.
  // video_playout requires run() on the application's main thread instead.
  // The optional stop token cancels both connection and the subsequent session.
  void join_with_token(const std::string& url, const std::string& token, RoomOptions options = {}, std::stop_token stop = {});
  void join(const std::string& url, const ConnectInfo& info, RoomOptions options = {});
  void disconnect();
  void wait(); // joins owned worker and rethrows asynchronous failures
  ConnectionState connection_state() const;
  RoomInfo info() const;
  int disconnect_reason() const;
  std::string last_error() const;
  LocalParticipant local_participant() const;
  std::vector<RemoteParticipant> remote_participants() const;
  std::optional<RemoteParticipant> participant_by_identity(const std::string& identity) const;
  std::optional<RemoteParticipant> participant_by_sid(const std::string& sid) const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Creates real native PeerConnection/data-channel SDP; throws on failure.
void webrtc_smoke(EventHandler handler = {});
std::unique_ptr<Room> connect_to_room_with_token(const std::string& url, const std::string& token,
    RoomCallback callbacks = {}, RoomOptions options = {});
std::unique_ptr<Room> connect_to_room(const std::string& url, const ConnectInfo& info,
    RoomCallback callbacks = {}, RoomOptions options = {});
}
