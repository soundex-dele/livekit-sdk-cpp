#include "engines/rtc/session.hpp"
#include <algorithm>

namespace livekit::sdk::detail {
void process_event(const std::shared_ptr<RoomState>& s, const Json& e) {
  std::vector<std::function<void()>> callbacks;
  std::deque<std::shared_ptr<Command>> interrupted;
  {
    std::lock_guard lock(s->mutex);
    auto& cb = s->callbacks; auto g = s->generation;
    const auto type = e.at("event").get<std::string>();
    auto tracks_changed = [&](const ParticipantInfo& old, const ParticipantInfo& now, bool local) {
      for (const auto& track : now.tracks) {
        auto previous = std::find_if(old.tracks.begin(), old.tracks.end(), [&](const auto& t) { return t.sid == track.sid; });
        TrackPublication pub(s, g, now.identity, track, local);
        if (previous == old.tracks.end()) {
          if (local && cb.on_local_track_published) callbacks.push_back([=, fn = cb.on_local_track_published] { fn(pub, LocalParticipant(s, g, now, true)); });
          if (!local && cb.on_track_published) callbacks.push_back([=, fn = cb.on_track_published] { fn(pub, RemoteParticipant(s, g, now, false)); });
        } else if (previous->muted != track.muted) {
          auto fn = track.muted ? cb.on_track_muted : cb.on_track_unmuted;
          if (fn) callbacks.push_back([=] { fn(pub, Participant(s, g, now, local)); });
        }
      }
      for (const auto& track : old.tracks) if (std::none_of(now.tracks.begin(), now.tracks.end(), [&](const auto& t) { return t.sid == track.sid; })) {
        TrackPublication pub(s, g, old.identity, track, local);
        if (!local && track.subscribed && cb.on_track_unsubscribed) callbacks.push_back([=, fn = cb.on_track_unsubscribed] { fn(pub, RemoteParticipant(s, g, old, false)); });
        s->subscriptions.erase(track.sid);
        if (local && cb.on_local_track_unpublished) callbacks.push_back([=, fn = cb.on_local_track_unpublished] { fn(pub, LocalParticipant(s, g, old, true)); });
        if (!local && cb.on_track_unpublished) callbacks.push_back([=, fn = cb.on_track_unpublished] { fn(pub, RemoteParticipant(s, g, old, false)); });
      }
    };
    if (type == "joined") {
      const auto& room = e.at("roomInfo");
      s->room = {room.value("sid", ""), room.value("name", ""), room.value("metadata", ""), room.value("recording", false)};
      s->local = parse_participant(e.at("localParticipant"));
      for (const auto& p : e.at("participants")) { auto info = parse_participant(p); s->remote[info.identity] = std::move(info); }
    } else if (type == "room-moved") {
      const auto& room = e.at("roomInfo");
      RoomMovedInfo moved{s->room, {room.value("sid", ""), room.value("name", ""),
          room.value("metadata", ""), room.value("recording", false)}};
      auto local = parse_participant(e.at("localParticipant"));
      std::map<std::string, ParticipantInfo> remote;
      std::map<std::string, bool> destination_subscriptions;
      std::set<std::string> participant_sids{local.sid}, track_sids;
      if (moved.current.sid.empty() || moved.current.name.empty() || local.sid.empty() || local.identity != s->local.identity)
        throw SDKError(ErrorDomain::protocol, ErrorCode::malformed_message, "Invalid room migration snapshot");
      for (auto& track : local.tracks) {
        if (track.sid.empty() || !track_sids.insert(track.sid).second)
          throw SDKError(ErrorDomain::protocol, ErrorCode::malformed_message, "Invalid migration publication snapshot");
        if (const auto id = s->recovery.local_ids.find(track.sid); id != s->recovery.local_ids.end()) track.client_id = id->second;
      }
      for (const auto& p : e.at("participants")) {
        auto info = parse_participant(p);
        if (info.state == 3) continue;
        if (info.identity.empty() || info.sid.empty() || info.identity == local.identity ||
            !participant_sids.insert(info.sid).second || remote.contains(info.identity))
          throw SDKError(ErrorDomain::protocol, ErrorCode::malformed_message, "Invalid migration participant snapshot");
        for (auto& track : info.tracks) {
          if (track.sid.empty() || !track_sids.insert(track.sid).second)
            throw SDKError(ErrorDomain::protocol, ErrorCode::malformed_message, "Invalid migration track snapshot");
          // OnTrack can precede the room snapshot on the native callback queue.
          // Retain observed subscription only for tracks in the destination.
          const auto observed = s->subscriptions.find(track.sid);
          if (observed != s->subscriptions.end() && observed->second) {
            track.subscribed = true; destination_subscriptions[track.sid] = true;
          }
        }
        remote.emplace(info.identity, std::move(info));
      }
      // Validate the complete snapshot before mutating any public state.
      for (const auto& [identity, old] : s->remote) {
        auto removed = old; removed.state = 3; removed.disconnect_reason = 8; removed.tracks.clear();
        tracks_changed(old, removed, false);
        if (cb.on_participant_disconnected) callbacks.push_back([=, fn = cb.on_participant_disconnected] {
          fn(RemoteParticipant(s, g, removed, false));
        });
      }
      interrupted.swap(s->commands); // Reject old queued work, not work enqueued by new callbacks.
      g = ++s->generation;
      s->room = moved.current; s->local = std::move(local); s->remote = std::move(remote);
      s->subscriptions = std::move(destination_subscriptions);
      s->recovery.local_snapshot = s->local;
      if (!e.value("transportRetained", true)) s->connection = ConnectionState::reconnecting;
      if (moved.previous.metadata != moved.current.metadata && cb.on_room_metadata_changed)
        callbacks.push_back([=, fn = cb.on_room_metadata_changed] { fn(moved.current.metadata); });
      if (moved.previous.recording != moved.current.recording && cb.on_recording_status_changed)
        callbacks.push_back([=, fn = cb.on_recording_status_changed] { fn(moved.current.recording); });
      if (cb.on_room_moved) callbacks.push_back([=, fn = cb.on_room_moved] { fn(moved); });
      for (const auto& [identity, now] : s->remote) {
        if (cb.on_participant_connected) callbacks.push_back([=, fn = cb.on_participant_connected] {
          fn(RemoteParticipant(s, g, now, false));
        });
        tracks_changed({}, now, false);
        for (const auto& track : now.tracks) if (track.subscribed && cb.on_track_subscribed)
          callbacks.push_back([=, fn = cb.on_track_subscribed] {
            fn(TrackPublication(s, g, identity, track, false), RemoteParticipant(s, g, now, false));
          });
      }
    } else if (type == "connected") {
      s->recovery.ever_connected = true;
      s->recovery.budget.reset();
      s->recovery.regions.attempted.clear();
      s->connection = ConnectionState::connected;
      s->cv.notify_all();
      if (cb.on_connected) callbacks.push_back(cb.on_connected);
    } else if (type == "reconnecting" || type == "reconnected") {
      const bool restored = type == "reconnected";
      s->connection = restored ? ConnectionState::connected : ConnectionState::reconnecting;
      ReconnectInfo info{e.value("mode", "resume") == "resume" ? ReconnectMode::resume : ReconnectMode::rejoin,
                         e.value("attempt", size_t{0})};
      auto fn = restored ? cb.on_reconnected : cb.on_reconnecting;
      if (restored) { s->recovery.budget.reset(); s->recovery.rejoining = false; s->recovery.regions.attempted.clear(); }
      if (fn) callbacks.push_back([=] { fn(info); });
      s->cv.notify_all();
    } else if (type == "participant") {
      auto now = parse_participant(e.at("participant"));
      const bool local = now.sid == s->local.sid;
      if (local) for (auto& t : now.tracks) {
        const auto id = s->recovery.local_ids.find(t.sid);
        if (id != s->recovery.local_ids.end()) t.client_id = id->second;
      }
      auto found = s->remote.find(now.identity);
      const bool existed = local || found != s->remote.end();
      auto old = local ? s->local : existed ? found->second : ParticipantInfo{};
      now.speaking = old.speaking; now.audio_level = old.audio_level;
      for (auto& track : now.tracks) track.subscribed = s->subscriptions[track.sid];
      if (now.state == 3) {
        if (!local && existed) {
          auto empty = now; empty.tracks.clear(); tracks_changed(old, empty, false);
          s->remote.erase(now.identity);
          if (cb.on_participant_disconnected) callbacks.push_back([=, fn = cb.on_participant_disconnected] { fn(RemoteParticipant(s, g, now, false)); });
        }
      } else {
        if (local) s->local = now; else s->remote[now.identity] = now;
        if (!local && !existed && cb.on_participant_connected) callbacks.push_back([=, fn = cb.on_participant_connected] { fn(RemoteParticipant(s, g, now, false)); });
        tracks_changed(old, now, local);
        if (existed && old.metadata != now.metadata && cb.on_metadata_changed) callbacks.push_back([=, fn = cb.on_metadata_changed] { fn(old.metadata, Participant(s, g, now, local)); });
        std::map<std::string, std::string> changed;
        for (const auto& [key, value] : now.attributes) { auto it = old.attributes.find(key); if (it == old.attributes.end() || it->second != value) changed[key] = value; }
        for (const auto& [key, value] : old.attributes) if (!now.attributes.contains(key)) changed[key] = "";
        if (existed && !changed.empty() && cb.on_attributes_changed) callbacks.push_back([=, fn = cb.on_attributes_changed] { fn(changed, Participant(s, g, now, local)); });
      }
    } else if (type == "track-published" || type == "track-unpublished" || type == "track-muted") {
      auto old = s->local;
      const auto sid = e.at("sid").get<std::string>();
      auto& tracks = s->local.tracks;
      if (type == "track-published") {
        auto t = parse_track(e.at("trackInfo"));
        auto it = std::find_if(tracks.begin(), tracks.end(), [&](const auto& existing) { return existing.sid == sid; });
        if (it == tracks.end()) tracks.push_back(t); else *it = t;
      } else if (type == "track-unpublished") std::erase_if(tracks, [&](const auto& t) { return t.sid == sid; });
      else for (auto& t : tracks) if (t.sid == sid) t.muted = e.at("muted");
      tracks_changed(old, s->local, true);
    } else if (type == "track-subscribed" || type == "track-unsubscribed") {
      const auto sid = e.at("track").get<std::string>(); const bool subscribed = type == "track-subscribed";
      const bool changed = s->subscriptions[sid] != subscribed;
      s->subscriptions[sid] = subscribed;
      for (auto& [identity, p] : s->remote) for (auto& t : p.tracks) if (t.sid == sid) {
        t.subscribed = subscribed;
        auto fn = subscribed ? cb.on_track_subscribed : cb.on_track_unsubscribed;
        if (changed && fn) callbacks.push_back([=] { fn(TrackPublication(s, g, identity, t, false), RemoteParticipant(s, g, p, false)); });
      }
    } else if (type == "data") {
      auto bytes = base64_decode(e.at("payloadBase64").get<std::string>());
      std::vector<uint8_t> payload(bytes.begin(), bytes.end());
      DataReceiveParams params{e.value("identity", ""), e.value("topic", ""), e.value("reliable", true)};
      if (cb.on_data_received) callbacks.push_back([=, fn = cb.on_data_received] { fn(payload, params); });
    } else if (type == "room-update") {
      auto metadata = e.value("metadata", ""); auto recording = e.value("recording", false);
      if (metadata != s->room.metadata && cb.on_room_metadata_changed) callbacks.push_back([=, fn = cb.on_room_metadata_changed] { fn(metadata); });
      if (recording != s->room.recording && cb.on_recording_status_changed) callbacks.push_back([=, fn = cb.on_recording_status_changed] { fn(recording); });
      s->room.metadata = metadata; s->room.recording = recording;
    } else if (type == "speakers") {
      if (e.value("replace", false)) { s->local.speaking = false; s->local.audio_level = 0; for (auto& [id, p] : s->remote) { p.speaking = false; p.audio_level = 0; } }
      for (const auto& speaker : e.at("speakers")) {
        auto apply = [&](ParticipantInfo& p) { if (p.sid == speaker.at("sid").get<std::string>()) { p.speaking = speaker.at("active"); p.audio_level = speaker.at("level"); } };
        apply(s->local); for (auto& [id, p] : s->remote) apply(p);
      }
      std::vector<Participant> active; std::map<std::string, float> levels;
      levels[s->local.sid] = s->local.audio_level; for (const auto& [id, p] : s->remote) levels[p.sid] = p.audio_level;
      if (s->local.speaking) active.emplace_back(s, g, s->local, true);
      for (const auto& [id, p] : s->remote) if (p.speaking) active.emplace_back(s, g, p, false);
      
      std::sort(active.begin(), active.end(), [&](const Participant& a, const Participant& b) { return levels[a.sid()] > levels[b.sid()]; });
      if (cb.on_active_speakers_changed) callbacks.push_back([=, fn = cb.on_active_speakers_changed] { fn(active); });
    } else if (type == "connection-quality") {
      if (cb.on_connection_quality_changed) callbacks.push_back([=, fn = cb.on_connection_quality_changed] { fn(e.at("sid"), e.at("quality")); });
    } else if (type == "subscription-failed") {
      if (cb.on_track_subscription_failed) callbacks.push_back([=, fn = cb.on_track_subscription_failed] { fn(e.at("sid"), e.at("error")); });
    } else if (type == "server-leave") s->reason = e.at("reason");
  }
  if (!interrupted.empty()) {
    const auto failure = std::make_exception_ptr(SDKError(ErrorDomain::lifecycle,
        ErrorCode::session_closed, "Command interrupted by room migration"));
    for (auto& command : interrupted) command->reject(failure);
  }
  for (const auto& callback : callbacks) callback();
}
}
