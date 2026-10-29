#include "session.hpp"
#include <set>
#include <chrono>
#include <algorithm>

namespace demo {
const char* state_name(State state) {
  switch (state) {
    case State::Idle:
      return "Idle";
    case State::Connecting:
      return "Connecting";
    case State::Connected:
      return "Connected";
    case State::Disconnecting:
      return "Disconnecting";
    default:
      return "Error";
  }
}

void Snapshot::update(Participant local, const std::vector<Participant>& remote) {
  std::set<std::string> current;
  participants.clear();
  local_sid = local.sid;
  if (!local.sid.empty())
    participants[local.sid] = std::move(local);
  for (const auto& p : remote) {
    if (p.state == 3)
      continue;
    participants[p.sid] = p;
    for (const auto& t : p.tracks)
      current.insert(t.sid);
  }
  // Unknown/removed tracks cannot accumulate pixels indefinitely. A frame that
  // precedes metadata is replaced by the next decoded frame once subscribed.
  std::erase_if(frames, [&](const auto& item) { return !current.contains(item.first); });
  std::erase_if(subscriptions, [&](const auto& sid) { return !current.contains(sid); });
  std::erase_if(quality.participants,
                [&](const auto& item) { return !participants.contains(item.first); });
  std::set<std::string> local_tracks;
  if (auto it = participants.find(local_sid); it != participants.end())
    for (const auto& track : it->second.tracks)
      local_tracks.insert(track.sid);
  std::erase_if(video_demand, [&](const auto& item) { return !local_tracks.contains(item.first); });
}

std::vector<Tile> tiles(const Snapshot& s) {
  std::vector<Tile> result;
  for (const auto& [sid, p] : s.participants) {
    if (sid == s.local_sid)
      continue;
    const auto label = p.name.empty() ? p.identity : p.name;
    bool video = false;
    for (const auto& t : p.tracks) {
      if (t.kind != rtc::PublicationKind::video)
        continue;
      video = true;
      Tile tile{t.sid,
                label + " / " + (t.name.empty() ? t.sid : t.name),
                t.muted ? "Video muted" : "Waiting for video",
                {}};
      if (!t.muted)
        if (auto f = s.frames.find(t.sid); f != s.frames.end()) {
          tile.frame = f->second;
          tile.status.clear();
        }
      result.push_back(std::move(tile));
    }
    if (!video)
      result.push_back({sid, label, "No video", {}});
  }
  return result;
}

bool Session::add_video() {
  std::lock_guard lock(mutex_);
  if (data_.state != State::Connected || data_.changing_video)
    return false;
  const auto local = data_.participants.find(data_.local_sid);
  if (local == data_.participants.end())
    return false;
  const auto& tracks = local->second.tracks;
  if (std::count_if(tracks.begin(), tracks.end(), [](const auto& t) {
        return t.kind == rtc::PublicationKind::video;
      }) >= max_test_videos)
    return false;
  video_command_ = VideoCommand{};
  data_.changing_video = true;
  data_.video_error.clear();
  return true;
}

bool Session::remove_video(const std::string& sid) {
  std::lock_guard lock(mutex_);
  if (data_.state != State::Connected || data_.changing_video)
    return false;
  const auto local = data_.participants.find(data_.local_sid);
  if (local == data_.participants.end())
    return false;
  const auto& tracks = local->second.tracks;
  if (std::none_of(tracks.begin(), tracks.end(), [&](const auto& t) {
        return t.sid == sid && t.kind == rtc::PublicationKind::video;
      }))
    return false;
  video_command_ = VideoCommand{sid};
  data_.changing_video = true;
  data_.video_error.clear();
  return true;
}

Snapshot Session::snapshot() const {
  std::lock_guard lock(mutex_);
  return data_;
}
}  // namespace demo
