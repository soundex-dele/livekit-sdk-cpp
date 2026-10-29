#include "livekit/player.hpp"
#include "media_graph/channel.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <thread>
#include <condition_variable>
#include <map>
#include <mutex>

using namespace livekit::sdk;
using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

template <class T>
T get(AsyncResult<T> future) {
  require(future.wait_for(5s) == std::future_status::ready, "control operation timed out");
  return future.get();
}

template <class F>
void fails(F&& operation) {
  bool failed = false;
  try {
    operation();
  } catch (const std::exception&) {
    failed = true;
  }
  require(failed, "expected operation to fail");
}

struct Counts {
  int video = 0, audio = 0;
  int64_t samples = 0, first_video = -1, last_video = -1;
  double energy = 0;
  int64_t first_audio = -1;
};

template <class T>
concept HasPollVideo = requires(T& value) { value.poll_video(); };
template <class T>
concept HasPollAudio = requires(T& value) { value.poll_audio(); };
static_assert(!HasPollVideo<Player> && !HasPollAudio<Player>);

// Install before playback. Media callbacks validate/count frames; the test waits
// for state notifications instead of driving SDK progress by draining queues.
class Collector {
 public:
  explicit Collector(Player& player, PlayerOutputOptions options = {}) : player_(player) {
    PlayerCallbacks callbacks;
    callbacks.video = [this](const media::VideoFrame& frame) {
      require(frame.rgba && frame.rgba->size() == size_t(frame.stride) * frame.height,
              "invalid RGBA output");
      const auto state = player_.snapshot();
      std::lock_guard lock(mutex_);
      ++calls_;
      if (state.state == PlayerState::paused) {
        previews_[frame.generation] = frame;
      } else {
        auto& count = counts_[frame.generation];
        require(frame.timestamp_us >= count.last_video, "nonmonotonic video timestamps");
        if (!count.video) {
          count.first_video = frame.timestamp_us;
        }
        count.last_video = frame.timestamp_us;
        ++count.video;
      }
      changed_.notify_all();
    };
    callbacks.audio = [this](const media::AudioFrame& frame) {
      require(frame.sample_rate == 48000 && frame.channels == 2 && frame.pcm &&
                  frame.pcm->size() == size_t(frame.samples) * 2,
              "invalid PCM output");
      require(player_.snapshot().state == PlayerState::playing, "audio outside playing state");
      std::lock_guard lock(mutex_);
      ++calls_;
      auto& count = counts_[frame.generation];
      if (!count.audio) {
        count.first_audio = frame.timestamp_us;
      }
      for (float value : *frame.pcm) {
        require(std::isfinite(value), "invalid PCM sample");
        count.energy += value * value;
      }
      ++count.audio;
      count.samples += frame.samples;
      changed_.notify_all();
    };
    callbacks.state = [this](const PlayerSnapshot& state) {
      std::lock_guard lock(mutex_);
      state_ = state;
      changed_.notify_all();
    };
    player_.set_callbacks(std::move(callbacks), options);
  }

  ~Collector() {
    player_.set_callbacks({});
  }

  Counts drain(int64_t lower_bound = 0) {
    const auto generation = player_.snapshot().generation;
    std::unique_lock lock(mutex_);
    require(changed_.wait_for(lock, 6s,
                              [&] {
                                return state_.generation == generation &&
                                       (state_.state == PlayerState::ended ||
                                        state_.state == PlayerState::failed);
                              }),
            "playback failed to reach drained EOS");
    require(state_.state != PlayerState::failed, state_.error.c_str());
    auto counts = counts_[generation];
    require(!counts.video || counts.first_video >= lower_bound, "stale video after seek");
    require(!counts.audio || counts.first_audio >= lower_bound, "stale audio after seek");
    return counts;
  }

  media::VideoFrame preview(uint64_t generation) {
    std::unique_lock lock(mutex_);
    require(changed_.wait_for(lock, 3s, [&] { return previews_.contains(generation); }),
            "paused seek did not deliver preview");
    return previews_.at(generation);
  }

  uint64_t calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }

  PlayerSnapshot wait_state(PlayerState value) {
    std::unique_lock lock(mutex_);
    require(changed_.wait_for(lock, 3s, [&] { return state_.state == value; }),
            "state callback timeout");
    return state_;
  }

 private:
  Player& player_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::map<uint64_t, Counts> counts_;
  std::map<uint64_t, media::VideoFrame> previews_;
  PlayerSnapshot state_;
  uint64_t calls_ = 0;
};

void check_demand(const std::string& path) {
  std::mutex mutex;
  std::condition_variable changed;
  int delivered = 0;
  Player player;
  PlayerCallbacks callbacks;
  callbacks.audio = [&](const auto&) {
    std::lock_guard lock(mutex);
    ++delivered;
    changed.notify_all();
  };
  player.set_callbacks(std::move(callbacks), {PlayerAudioDelivery::on_demand});
  get(player.open(path));
  get(player.play());
  auto generation = player.snapshot().generation;
  {
    std::unique_lock lock(mutex);
    require(!changed.wait_for(lock, 60ms, [&] { return delivered != 0; }),
            "audio delivered without device credit");
  }
  require(!player.request_audio(generation + 1), "stale audio credit accepted");
  require(player.request_audio(generation), "current audio credit rejected");
  {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, 3s, [&] { return delivered == 1; }),
            "audio credit not delivered");
    require(!changed.wait_for(lock, 40ms, [&] { return delivered > 1; }), "credit delivered twice");
  }
  get(player.pause());
  require(!player.request_audio(generation), "paused player accepted audio credit");
  get(player.seek(0us));
  get(player.play());
  require(!player.request_audio(generation), "seek retained stale audio credit");
  // The outstanding audio branch cannot prevent lifecycle progress.
  get(player.close());
  player.set_callbacks({});
}

void check_callback_fences(const std::string& path) {
  for (bool close : {false, true}) {
    std::promise<void> entered, release;
    auto entered_result = entered.get_future();
    auto release_result = release.get_future().share();
    Player player;
    PlayerCallbacks callbacks;
    callbacks.video = [&](const auto&) {
      player.set_callbacks({});  // Self-removal does not erase the active invocation fence.
      entered.set_value();
      require(release_result.wait_for(3s) == std::future_status::ready, "callback release timeout");
    };
    player.set_callbacks(std::move(callbacks));
    get(player.open(path));
    get(player.play());
    require(entered_result.wait_for(3s) == std::future_status::ready, "callback did not start");
    auto removal = std::async(std::launch::async, [&] { player.set_callbacks({}); });
    auto lifecycle = close ? player.close() : player.pause();
    const bool removal_waited = removal.wait_for(40ms) == std::future_status::timeout;
    const bool lifecycle_waited = lifecycle.wait_for(40ms) == std::future_status::timeout;
    release.set_value();
    removal.get();
    get(std::move(lifecycle));
    require(removal_waited && lifecycle_waited, "lifecycle/removal skipped active callback fence");
    get(player.close());
  }
}

void check_callback_failure(const std::string& path) {
  std::promise<PlayerSnapshot> failed;
  auto result = failed.get_future();
  const SDKError cause(ErrorDomain::media, ErrorCode::callback_failed, "consumer failed");
  Player player;
  PlayerCallbacks callbacks;
  callbacks.video = [cause](const auto&) { throw cause; };
  callbacks.state = [&](const auto& state) {
    if (state.state == PlayerState::failed) {
      failed.set_value(state);
    }
  };
  player.set_callbacks(std::move(callbacks));
  get(player.open(path));
  get(player.play());
  require(result.wait_for(3s) == std::future_status::ready, "callback failure was not reported");
  const auto state = result.get();
  require(state.failure && state.failure->id == cause.failure()->id &&
              player.snapshot().failure->id == state.failure->id,
          "callback failure correlation lost");
  player.set_callbacks({});
  get(player.close());
}
}  // namespace

int main(int argc, char** argv) {
  try {
    require(argc == 2, "fixture directory required");
    const std::filesystem::path root = argv[1];
    const auto file = [&](const char* name) {
      auto text = (root / name).u8string();
      return std::string(text.begin(), text.end());
    };
    PlayerOptions options;
    options.video_queue_frames = 2;
    options.audio_queue_frames = 3;
    Player player(options);
    Collector collected(player);
    fails([&] { get(player.play()); });
    fails([&] { get(player.open(file("missing.mp4"))); });
    require(player.snapshot().state == PlayerState::failed, "open failure state");
    fails([&] { get(player.open(file("invalid.mp4"))); });
    fails([&] { get(player.open("https://example.invalid/video.mp4")); });
    auto info = get(player.open(file("av.mp4")));
    require(info.has_audio && info.has_video && info.width == 160 && info.height == 90,
            "probe failed");
    require(info.duration_us >= 2300000 && info.duration_us <= 2500000, "incorrect duration");
    get(player.play());
    std::this_thread::sleep_for(150ms);  // Playback progresses without calling a media-read API.
    auto buffered = player.snapshot();
    require(buffered.queued_video <= 2 && buffered.queued_audio <= 3, "unbounded queue");
    get(player.pause());
    auto paused = player.snapshot().position_us;
    const auto paused_calls = collected.calls();
    std::this_thread::sleep_for(40ms);
    require(player.snapshot().position_us == paused, "clock advanced while paused");
    require(collected.calls() == paused_calls, "output while paused");
    fails([&] { get(player.seek(-1us)); });
    fails([&] { get(player.seek(10s)); });
    require(player.snapshot().state == PlayerState::paused, "invalid seek changed state");
    auto first = player.seek(1500ms);
    auto second = player.seek(400ms);
    auto third = player.seek(1800ms);
    get(std::move(first));
    get(std::move(second));
    get(std::move(third));
    require(player.snapshot().generation > buffered.generation, "seek did not advance generation");
    const auto preview = collected.preview(player.snapshot().generation);
    require(preview.timestamp_us >= 1800000 && preview.timestamp_us < 1840000,
            "paused seek did not deliver target frame");
    require(preview.generation == player.snapshot().generation, "stale preview generation");
    const auto preview_calls = collected.calls();
    std::this_thread::sleep_for(40ms);
    require(player.snapshot().position_us == 1800000 && collected.calls() == preview_calls,
            "preview advanced clock or delivered more than one frame");
    get(player.play());
    auto tail = collected.drain(1800000);
    require(tail.video == 15 && tail.audio > 0, "seek did not drain final B frames/audio");
    get(player.play());  // replay at EOS
    auto full = collected.drain();
    require(full.video == 60 && full.audio > 0 && full.energy > 1, "full playback decode mismatch");
    require(full.samples >= 115000 && full.samples <= 117000, "resampled audio duration mismatch");
    get(player.stop());
    require(player.snapshot().state == PlayerState::ready && player.snapshot().position_us == 0,
            "stop failed");
    get(player.play());
    get(player.seek(2s));  // preserve the playing state across seek
    require(player.snapshot().state == PlayerState::playing, "playing seek paused unexpectedly");
    const auto playing_tail = collected.drain(2000000);
    if (playing_tail.video != 10)
      throw std::runtime_error("playing seek tail mismatch: " + std::to_string(playing_tail.video) +
                               " first=" + std::to_string(playing_tail.first_video));
    const auto end_calls = collected.calls();
    get(player.seek(std::chrono::microseconds(info.duration_us)));
    std::this_thread::sleep_for(40ms);
    require(collected.calls() == end_calls && player.snapshot().state == PlayerState::paused,
            "end preview returned stale output or changed state");
    get(player.play());
    require(collected.drain(info.duration_us).video == 0, "seek to duration returned old frames");
    get(player.close());
    get(player.close());
    require(player.snapshot().state == PlayerState::closed, "idempotent close failed");
    for (const char* name : {"audio.wav", "video.mp4", "offset.ts"}) {
      auto stream = get(player.open(file(name)));
      get(player.seek(0us));
      get(player.play());
      auto count = collected.drain();
      if ((count.video > 0) != stream.has_video || (count.audio > 0) != stream.has_audio)
        throw std::runtime_error(std::string("single-stream playback failed: ") + name +
                                 " video=" + std::to_string(count.video) +
                                 " audio=" + std::to_string(count.audio));
      if (stream.has_video)
        require(count.video == 10 && count.first_video == 0,
                "start timestamp normalization/B-frame drain failed");
      if (stream.has_audio)
        require(std::abs(count.samples - 16800) <= 1, "resampler tail lost");
    }
    auto unicode = root / std::filesystem::u8path("本地 播放.wav");
    std::filesystem::copy_file(root / "audio.wav", unicode,
                               std::filesystem::copy_options::overwrite_existing);
    auto utf8 = unicode.u8string();
    get(player.open(std::string(utf8.begin(), utf8.end())));
    get(player.play());
    player.cancel_pending_io();
    std::this_thread::sleep_for(50ms);
    get(player.close());
    // Control responsiveness while backpressured, and independent session ownership.
    Player other;
    Collector other_collected(other);
    get(other.open(file("audio.wav")));
    get(other.play());
    get(player.open(file("av.mp4")));
    get(player.play());
    std::this_thread::sleep_for(100ms);
    get(player.close());
    require(other_collected.drain().audio > 0, "closing one player affected another");
    PlayerOptions tiny;
    tiny.queue_bytes = 16;
    Player limited(tiny);
    Collector limited_collected(limited);
    get(limited.open(file("video.mp4")));
    get(limited.play());
    limited_collected.wait_state(PlayerState::failed);
    require(limited.snapshot().state == PlayerState::failed,
            "oversized frame did not fail boundedly");
    check_demand(file("audio.wav"));
    check_callback_fences(file("video.mp4"));
    check_callback_failure(file("video.mp4"));
    std::cout << "PASS: probe, controls, bounded queues, seek generations, EOS, B frames, PCM, "
                 "UTF-8, errors, session isolation\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
