#pragma once
#include "livekit/rtp.hpp"
#include "livekit/auth.hpp"
#include "livekit/media.hpp"
#include "livekit/participant.hpp"
#include "livekit/reconnect.hpp"
#include "livekit/token_provider.hpp"
#include "livekit/video_stream.hpp"
#include "livekit/audio_stream.hpp"
#include "livekit/capabilities.hpp"
#include "livekit/encryption.hpp"
#include "livekit/encoded_media.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <optional>

namespace livekit::sdk {
using EventHandler = std::function<void(const Json&)>;
enum class ConnectionState { disconnected, connecting, connected, reconnecting };
// Applies to both media PeerConnections, including ICE restart and full rejoin.
// Strict modes require direct ICE and never use TURN or the other protocol.
enum class MediaTransport { automatic, udp_only, tcp_only };
struct RoomInfo { std::string sid, name, metadata; bool recording = false; };
struct RoomMovedInfo { RoomInfo previous, current; };
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
  std::function<void(const ReconnectInfo&)> on_reconnecting;
  std::function<void(const ReconnectInfo&)> on_reconnected;
  // State already names the destination. Reacquire Participant/Publication
  // handles; a room boundary invalidates old handles even if media continues.
  // The new server token is retained internally, never included in events.
  std::function<void(const RoomMovedInfo&)> on_room_moved;
  std::function<void(const DtmfDigit&, const DataReceiveParams&)> on_dtmf_received;
  // Each event is an update, not accumulated text. Merge by sender/track/segment
  // ID in the application. params.sender_identity is distinct from the subject.
  std::function<void(const Transcription&, const DataReceiveParams&)> on_transcription_received;
  std::function<void(const std::string& participant_sid, const std::string& track_sid, bool allowed)> on_subscription_permission_changed;
  // Statistics enabled by RoomOptions::stats_interval_ms. Inspect "event":
  // rtc-stats contains W3C reports, graph-track-stats contains track counters,
  // media-graph-stats contains node/edge counters and monotonic timestampUs.
  std::function<void(const Json&)> on_stats;
  std::function<void(const RemoteDataTrack&)> on_data_track_published, on_data_track_unpublished;
  std::function<void(const EncryptionStateEvent&)> on_encryption_state_changed;
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
  // Latest remote frame per track, plus local frames when MediaSpec::local_preview is enabled.
  // Serialized with events on the run thread; local preview also supports subscribe=false.
  // Copy the frame to retain it. Exceptions terminate and clean up the session.
  VideoFrameHandler video_frame_handler;
  ReconnectOptions reconnect;
  // Used when the initial token is empty, or once after an HTTP 401 on a
  // connection attempt. Server refresh tokens remain preferred. A 403 does not
  // trigger refresh. Provider time consumes the connection/recovery budget.
  TokenProvider token_provider;
  std::chrono::milliseconds token_timeout{10000};
  RegionOptions regions;
  // 12 preserves the baseline protocol. Opt into 15 for successful metadata
  // RequestResponse acknowledgements. Other protocol versions are rejected.
  int signal_protocol = 12;
  // Bounded pull output, independent of application processing speed. Choose
  // either this or video_frame_handler. Closed at logical session termination.
  std::shared_ptr<VideoFrameStream> video_frame_stream;
  // Decoded per-track PCM before device mixing/playout. Audio/video streams
  // may be used together; each is bound to this logical session once.
  std::shared_ptr<AudioFrameStream> audio_frame_stream;
  // Applied before publishing any initial/recovered track. Empty uses server default.
  std::optional<SubscriptionPermissions> subscription_permissions;
  MediaTransport media_transport = MediaTransport::automatic;
  std::shared_ptr<EncodedMediaStream> encoded_frame_stream;
  EncryptionOptions encryption;
  // Dedicated ordered callback worker. Queue overflow terminates the session.
  // wait()/blocking join from this worker is forbidden; request_stop is safe.
  bool control_callback_thread=false;
  size_t control_callback_capacity=256;
  std::shared_ptr<PacketStream> packet_stream;
  bool ipv6_only=false; // Restricts media ICE interfaces; signaling follows its URL.
  bool synchronize_av=true;
  bool decode_video=true; // false retains encoded output without software video decoding.
  std::function<void(const Json&)> log_sink; // Ordered SDK diagnostics; no key material.
  bool single_peer_connection=false; // Requires protocol 17 and /rtc/v1.
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
  // Registrations survive reconnect and explicit Room reuse. Duplicate methods
  // are rejected; unregister affects new calls, not already dispatched work.
  void register_rpc_method(const std::string& method, RpcHandler handler);
  void unregister_rpc_method(const std::string& method);
  void register_text_stream_handler(const std::string& topic, StreamHandler handler);
  void register_byte_stream_handler(const std::string& topic, StreamHandler handler);
  void unregister_text_stream_handler(const std::string& topic);
  void unregister_byte_stream_handler(const std::string& topic);
  std::vector<RemoteDataTrack> remote_data_tracks() const;
  std::vector<LocalDataTrack> local_data_tracks() const;
  ConnectionState connection_state() const;
  // Current-generation snapshot. Server codec lists are advertised policy,
  // not proof that a track negotiated or decoded a codec.
  SessionCapabilities capabilities() const;
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
