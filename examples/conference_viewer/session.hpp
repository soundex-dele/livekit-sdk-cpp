#pragma once
#include <livekit/rtc/types.hpp>
#include <livekit/media.hpp>
#include "quality.hpp"
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <filesystem>
#include <set>

namespace demo {
namespace sdk = livekit::sdk;
namespace rtc = sdk::rtc;

struct SessionOptions {
  rtc::RoomOptions connection;
  std::string room;
  std::vector<sdk::MediaSpec> media;
  bool audio_playout = false;
  std::filesystem::path record;
  std::function<void(const sdk::Json&)> log_sink;
};

// UI model: advertised publications, independent of the transport's handles.
struct Participant {
  std::string sid, identity, name;
  int state = 0;
  std::vector<rtc::PublicationInfo> tracks;
};

inline constexpr int max_test_videos = 8;
enum class State { Idle, Connecting, Connected, Disconnecting, Error };
const char* state_name(State state);

struct Snapshot {
  State state = State::Idle;
  bool finished = true;
  std::string error, room, local_sid;
  bool changing_video = false;
  bool pause_unused_video = false;
  std::map<std::string, bool> video_demand;
  std::string video_error;
  std::map<std::string, Participant> participants;
  std::set<std::string>
      subscriptions;  // Confirmed graph bindings, separate from advertised publications.
  std::map<std::string, std::shared_ptr<const sdk::VideoFrame>> frames;
  uint64_t received_frames = 0;
  QualitySnapshot quality;
  void update(Participant local, const std::vector<Participant>& remote);
};

struct Tile {
  std::string id, label, status;
  std::shared_ptr<const sdk::VideoFrame> frame;
};

std::vector<Tile> tiles(const Snapshot& snapshot);

class Session {
 public:
  Session();
  ~Session();
  void start(SessionOptions options, bool pause_unused_video = false);
  void stop();
  // Queue one operation for the supervisor; never wait on the UI thread.
  bool add_video();
  bool remove_video(const std::string& sid);
  Snapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  Snapshot data_;
  std::jthread worker_;

  struct VideoCommand {
    std::string remove_sid;
  };  // empty SID adds a test video

  std::optional<VideoCommand> video_command_;
};
}  // namespace demo
