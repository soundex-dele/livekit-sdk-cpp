#pragma once
#include <livekit/livekit.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace demo {
namespace sdk = livekit::sdk;
inline constexpr int max_test_videos = 8;
enum class State { Idle, Connecting, Connected, Disconnecting, Error };
const char* state_name(State state);
struct Snapshot {
  State state = State::Idle;
  bool finished = true;
  std::string error, room, local_sid;
  bool changing_video = false;
  std::string video_error;
  std::map<std::string, sdk::ParticipantInfo> participants;
  std::map<std::string, std::shared_ptr<const sdk::VideoFrame>> frames;
  uint64_t received_frames = 0;
  void update(sdk::ParticipantInfo local, const std::vector<sdk::ParticipantInfo>& remote);
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
  void start(sdk::RoomOptions options);
  void stop();
  // Queue one operation for the supervisor; never wait on the UI thread.
  bool add_video();
  bool remove_video(const std::string& sid);
  Snapshot snapshot() const;
 private:
  mutable std::mutex mutex_;
  Snapshot data_;
  sdk::Room room_;
  std::jthread worker_;
  struct VideoCommand { std::string remove_sid; }; // empty SID adds a test video
  std::optional<VideoCommand> video_command_;
};
}
