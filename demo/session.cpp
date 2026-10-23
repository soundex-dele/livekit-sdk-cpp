#include "session.hpp"
#include <set>
#include <chrono>

namespace demo {
const char* state_name(State state) {
  switch (state) {
    case State::Idle: return "Idle"; case State::Connecting: return "Connecting";
    case State::Connected: return "Connected"; case State::Disconnecting: return "Disconnecting";
    default: return "Error";
  }
}
void Snapshot::update(sdk::ParticipantInfo local, const std::vector<sdk::ParticipantInfo>& remote) {
  std::set<std::string> current;
  participants.clear(); local_sid = local.sid;
  if (!local.sid.empty()) participants[local.sid] = std::move(local);
  for (const auto& p : remote) {
    if (p.state == 3) continue;
    participants[p.sid] = p;
    for (const auto& t : p.tracks) if (t.subscribed) current.insert(t.sid);
  }
  // Unknown/removed tracks cannot accumulate pixels indefinitely. A frame that
  // precedes metadata is replaced by the next decoded frame once subscribed.
  std::erase_if(frames, [&](const auto& item) { return !current.contains(item.first); });
}
std::vector<Tile> tiles(const Snapshot& s) {
  std::vector<Tile> result;
  for (const auto& [sid, p] : s.participants) {
    if (sid == s.local_sid) continue;
    const auto label = p.name.empty() ? p.identity : p.name;
    bool video = false;
    for (const auto& t : p.tracks) {
      if (t.kind != sdk::TrackKind::video) continue;
      video = true;
      Tile tile{t.sid, label, t.muted ? "Video muted" : "Waiting for video", {}};
      if (!t.muted) if (auto f = s.frames.find(t.sid); f != s.frames.end()) { tile.frame = f->second; tile.status.clear(); }
      result.push_back(std::move(tile));
    }
    if (!video) result.push_back({sid, label, "No video", {}});
  }
  return result;
}
Session::Session() : room_([this] {
  sdk::RoomCallback callbacks;
  callbacks.on_track_unsubscribed = [this](const sdk::TrackPublication& t, const sdk::RemoteParticipant&) {
    std::lock_guard lock(mutex_); data_.frames.erase(t.sid());
  };
  return callbacks;
}()) {}
Session::~Session() { stop(); if (worker_.joinable()) worker_.join(); }
void Session::start(sdk::RoomOptions options) {
  { std::lock_guard lock(mutex_); if (!data_.finished) throw std::logic_error("Session already active"); }
  if (worker_.joinable()) worker_.join();
  { std::lock_guard lock(mutex_); data_ = {}; data_.state = State::Connecting; data_.finished = false; data_.room = options.room; }
  options.video_playout = false;
  options.video_frame_handler = [this](const sdk::VideoFrame& frame) {
    auto owned = std::make_shared<sdk::VideoFrame>(frame);
    std::lock_guard lock(mutex_);
    if (data_.state != State::Disconnecting) { data_.frames[frame.track_id] = std::move(owned); ++data_.received_frames; }
  };
  try {
    worker_ = std::jthread([this, options = std::move(options)](std::stop_token stop) mutable {
      std::string error;
      try {
        // Blocking joins/futures belong on this supervisor, never the UI or SDK callback thread.
        auto media = std::move(options.media); options.media.clear();
        room_.join_with_token(options.url, options.token, options, stop);
        for (const auto& spec : media) {
          if (stop.stop_requested()) break;
          room_.local_participant().publish_track(spec, {spec.video ? "Test video" : "Test tone"}).get();
        }
        while (!stop.stop_requested() && room_.connection_state() != sdk::ConnectionState::disconnected) {
          auto local = room_.local_participant().info();
          std::vector<sdk::ParticipantInfo> remote;
          for (const auto& p : room_.remote_participants()) remote.push_back(p.info());
          const auto name = room_.info().name;
          { std::lock_guard lock(mutex_);
            data_.update(std::move(local), remote); data_.room = name;
            if (data_.state != State::Disconnecting) data_.state = State::Connected;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (stop.stop_requested()) room_.request_stop();
        room_.wait();
        if (!stop.stop_requested()) error = "Disconnected by server (reason " + std::to_string(room_.disconnect_reason()) + "). Join again to reconnect.";
      } catch (const std::exception& e) { error = e.what(); }
      catch (...) { error = "Unknown session failure"; }
      room_.disconnect();
      if (!options.token.empty()) for (size_t pos; (pos = error.find(options.token)) != std::string::npos;) error.replace(pos, options.token.size(), "[redacted]");
      std::lock_guard lock(mutex_);
      if (stop.stop_requested()) error.clear();
      data_.state = error.empty() ? State::Idle : State::Error;
      data_.error = std::move(error); data_.finished = true;
      data_.frames.clear(); data_.participants.clear(); data_.local_sid.clear();
    });
  } catch (...) { std::lock_guard lock(mutex_); data_.finished = true; data_.state = State::Error; throw; }
}
void Session::stop() {
  { std::lock_guard lock(mutex_); if (data_.finished) return; data_.state = State::Disconnecting; }
  worker_.request_stop(); room_.request_stop();
}
Snapshot Session::snapshot() const { std::lock_guard lock(mutex_); return data_; }
}
