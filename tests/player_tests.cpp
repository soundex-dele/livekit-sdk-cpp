#include "livekit/player.hpp"
#include "media_graph/channel.hpp"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

template <class T>
T get(std::future<T> future) {
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
};

Counts drain(Player& player, int64_t lower_bound = 0) {
  Counts counts;
  const auto generation = player.snapshot().generation;
  const auto deadline = std::chrono::steady_clock::now() + 6s;
  while (std::chrono::steady_clock::now() < deadline) {
    while (auto frame = player.poll_video()) {
      require(frame->generation == generation && frame->timestamp_us >= lower_bound,
              "stale video after seek");
      require(
          frame->rgba && frame->rgba->size() == static_cast<size_t>(frame->stride) * frame->height,
          "invalid RGBA output");
      require(frame->timestamp_us >= counts.last_video, "nonmonotonic video timestamps");
      if (!counts.video)
        counts.first_video = frame->timestamp_us;
      counts.last_video = frame->timestamp_us;
      ++counts.video;
    }
    while (auto frame = player.poll_audio()) {
      require(frame->generation == generation && frame->timestamp_us >= lower_bound,
              "stale audio after seek");
      require(frame->sample_rate == 48000 && frame->channels == 2 &&
                  frame->pcm->size() == static_cast<size_t>(frame->samples) * 2,
              "invalid PCM output");
      for (float value : *frame->pcm) {
        require(std::isfinite(value), "invalid PCM sample");
        counts.energy += value * value;
      }
      ++counts.audio;
      counts.samples += frame->samples;
    }
    auto state = player.snapshot();
    require(state.state != PlayerState::failed, state.error.c_str());
    if (state.state == PlayerState::ended)
      return counts;
    std::this_thread::sleep_for(2ms);
  }
  throw std::runtime_error("playback failed to reach drained EOS");
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
    std::this_thread::sleep_for(150ms);  // deliberately slow consumer, queues must stay bounded
    auto buffered = player.snapshot();
    require(buffered.queued_video <= 2 && buffered.queued_audio <= 3, "unbounded queue");
    get(player.pause());
    auto paused = player.snapshot().position_us;
    std::this_thread::sleep_for(40ms);
    require(player.snapshot().position_us == paused, "clock advanced while paused");
    require(!player.poll_video() && !player.poll_audio(), "output while paused");
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
    std::optional<media::VideoFrame> preview;
    const auto preview_limit = std::chrono::steady_clock::now() + 3s;
    while (!(preview = player.poll_video()) && std::chrono::steady_clock::now() < preview_limit) {
      require(!player.poll_audio(), "preview emitted audio");
      require(player.snapshot().state == PlayerState::paused, "preview changed playback state");
      std::this_thread::sleep_for(2ms);
    }
    require(preview && preview->timestamp_us >= 1800000 && preview->timestamp_us < 1840000,
            "paused seek did not deliver target frame");
    require(preview->generation == player.snapshot().generation, "stale preview generation");
    std::this_thread::sleep_for(40ms);
    require(
        player.snapshot().position_us == 1800000 && !player.poll_audio() && !player.poll_video(),
        "preview advanced clock or delivered more than one frame");
    get(player.play());
    auto tail = drain(player, 1800000);
    require(tail.video == 15 && tail.audio > 0, "seek did not drain final B frames/audio");
    get(player.play());  // replay at EOS
    auto full = drain(player);
    require(full.video == 60 && full.audio > 0 && full.energy > 1, "full playback decode mismatch");
    require(full.samples >= 115000 && full.samples <= 117000, "resampled audio duration mismatch");
    get(player.stop());
    require(player.snapshot().state == PlayerState::ready && player.snapshot().position_us == 0,
            "stop failed");
    get(player.play());
    get(player.seek(2s));  // preserve the playing state across seek
    require(player.snapshot().state == PlayerState::playing, "playing seek paused unexpectedly");
    const auto playing_tail = drain(player, 2000000);
    if (playing_tail.video != 10)
      throw std::runtime_error("playing seek tail mismatch: " + std::to_string(playing_tail.video) +
                               " first=" + std::to_string(playing_tail.first_video));
    get(player.seek(std::chrono::microseconds(info.duration_us)));
    std::this_thread::sleep_for(40ms);
    require(!player.poll_video() && !player.poll_audio() &&
                player.snapshot().state == PlayerState::paused,
            "end preview returned stale output or changed state");
    get(player.play());
    require(drain(player, info.duration_us).video == 0, "seek to duration returned old frames");
    get(player.close());
    get(player.close());
    require(player.snapshot().state == PlayerState::closed, "idempotent close failed");
    for (const char* name : {"audio.wav", "video.mp4", "offset.ts"}) {
      auto stream = get(player.open(file(name)));
      get(player.seek(0us));
      get(player.play());
      auto count = drain(player);
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
    get(other.open(file("audio.wav")));
    get(other.play());
    get(player.open(file("av.mp4")));
    get(player.play());
    std::this_thread::sleep_for(100ms);
    get(player.close());
    require(drain(other).audio > 0, "closing one player affected another");
    PlayerOptions tiny;
    tiny.queue_bytes = 16;
    Player limited(tiny);
    get(limited.open(file("video.mp4")));
    get(limited.play());
    for (int i = 0; i < 100 && limited.snapshot().state != PlayerState::failed; ++i)
      std::this_thread::sleep_for(5ms);
    require(limited.snapshot().state == PlayerState::failed,
            "oversized frame did not fail boundedly");
    std::cout << "PASS: probe, controls, bounded queues, seek generations, EOS, B frames, PCM, "
                 "UTF-8, errors, session isolation\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
