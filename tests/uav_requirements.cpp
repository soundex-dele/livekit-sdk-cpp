// Headless C++ clients for the Go SFU requirement assessment. No capture devices.
#include <livekit/livekit.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <numbers>
#include <thread>

using namespace livekit::sdk;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
static void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
template<class F> void until(F fn, const std::string& message) {
  const auto end = Clock::now() + 15s;
  while (!fn()) {
    require(Clock::now() < end, message);
    std::this_thread::sleep_for(20ms);
  }
}
template<class T> T done(std::future<T> f) {
  require(f.wait_for(15s) == std::future_status::ready, "SDK operation timed out");
  return f.get();
}
static std::string token(const std::string& identity) {
  return AccessToken("uav-test", "uav-local-test-secret-at-least-32-characters")
    .set_identity(identity).set_video_grant({{"roomJoin", true}, {"room", "uav-requirements"},
      {"canPublish", true}, {"canSubscribe", true}, {"canPublishData", true}}).to_jwt();
}

struct VideoObservation { uint64_t frames = 0; int width = 0, height = 0; double y = 0; };
struct Peer {
  // The worker is joined before any state used by callbacks is destroyed.
  std::mutex mutex;
  std::map<std::string, VideoObservation> video;
  std::shared_ptr<AudioFrameStream> audio = std::make_shared<AudioFrameStream>(AudioStreamOptions{500});
  Room room;
  std::ofstream events;
  std::exception_ptr failure;
  std::atomic<bool> finished{false};
  std::jthread worker;
  Peer(const std::string& url, const std::string& identity, const std::string& codec,
       const std::filesystem::path& directory) : events(directory / (identity + ".jsonl")) {
    RoomOptions o; o.url = url; o.token = token(identity); o.codec = codec;
    o.disable_turn = true; o.stats_interval_ms = 1000; o.audio_frame_stream = audio;
    o.video_frame_handler = [this](const VideoFrame& f) {
      std::lock_guard lock(mutex);
      auto& v = video[f.track_id]; ++v.frames; v.width = f.width; v.height = f.height;
      uint64_t sum = 0;
      for (int y = 0; y < f.height; ++y)
        for (int x = 0; x < f.width; ++x) sum += f.y.at(y * f.stride_y + x);
      v.y = double(sum) / (f.width * f.height);
    };
    worker = std::jthread([this, o](std::stop_token stop) {
      try {
        room.run(o, [this](const Json& event) {
          // Never retain SDP or access tokens in this evidence file.
          if (event.value("event", "") == "rtc-stats") events << event.dump() << '\n';
        }, stop);
      } catch (...) { failure = std::current_exception(); }
      finished.store(true);
    });
    until([&] { return finished.load() || room.connection_state() == ConnectionState::connected; }, "join timeout: " + identity);
    if (finished.load()) { worker.join(); if (failure) std::rethrow_exception(failure); }
  }
  ~Peer() { worker.request_stop(); room.request_stop(); if (worker.joinable()) worker.join(); }
  VideoObservation observation(const std::string& sid) {
    std::lock_guard lock(mutex); return video[sid];
  }
  TrackPublication remote(const std::string& who, const std::string& sid) {
    std::optional<TrackPublication> publication;
    until([&] {
      auto p = room.participant_by_identity(who); if (!p) return false;
      for (const auto& t : p->track_publications()) if (t.sid() == sid) publication = t;
      return publication.has_value();
    }, "missing remote publication: " + sid);
    return *publication;
  }
};

struct VideoInput {
  std::shared_ptr<I420VideoSource> source = std::make_shared<I420VideoSource>(I420SourceOptions{320, 180});
  std::jthread writer;
  explicit VideoInput(int luma) : writer([this, luma](std::stop_token stop) {
    VideoFrame f; f.width = 320; f.height = 180; f.stride_y = 320; f.stride_u = f.stride_v = 160;
    f.y.assign(320 * 180, luma); f.u.assign(160 * 90, 100); f.v.assign(160 * 90, 160);
    while (!stop.stop_requested()) { source->write(f); std::this_thread::sleep_for(33ms); }
  }) {}
  ~VideoInput() { writer.request_stop(); writer.join(); source->close(); }
  TrackPublication publish(Peer& p, const std::string& name) {
    MediaSpec m; m.width = 320; m.height = 180; m.video_source = source;
    return done(p.room.local_participant().publish_track(m, {name, "device-01", TrackSource::camera}));
  }
};
struct AudioInput {
  std::shared_ptr<PcmAudioSource> source = std::make_shared<PcmAudioSource>();
  std::jthread writer;
  explicit AudioInput(int hz) : writer([this, hz](std::stop_token stop) {
    uint64_t offset = 0;
    while (!stop.stop_requested()) {
      PcmInputFrame f; f.samples.resize(480);
      for (size_t i = 0; i < f.samples.size(); ++i)
        f.samples[i] = static_cast<int16_t>(6000 * std::sin(2 * std::numbers::pi * hz * (offset + i) / 48000));
      if (source->write(f) == AudioWriteStatus::accepted) offset += f.samples.size();
      std::this_thread::sleep_for(2ms);
    }
  }) {}
  ~AudioInput() { writer.request_stop(); writer.join(); source->close(); }
  TrackPublication publish(Peer& p, const std::string& name) {
    MediaSpec m; m.video = false; m.audio_source = source;
    TrackPublishOptions o; o.name = name; o.source = TrackSource::microphone; o.disable_red = true;
    return done(p.room.local_participant().publish_track(m, o));
  }
};
struct Tone {
  uint64_t count = 0, crossings = 0; int previous = 0, rate = 0, peak = 0;
  void add(const AudioFrame& f) {
    require(f.format == AudioSampleFormat::pcm16 && f.channels > 0, "invalid PCM");
    rate = f.sample_rate;
    for (size_t i = 0; i < f.samples_per_channel; ++i) {
      const int sample = f.samples.at(i * f.channels);
      if (previous < 0 && sample >= 0) ++crossings;
      previous = sample; peak = std::max(peak, std::abs(sample)); ++count;
    }
  }
  double hz() const { return count ? double(crossings) * rate / count : 0; }
};
static void verify_video(Peer& peer, const std::string& sid, int luma, uint64_t after = 0) {
  until([&] { return peer.observation(sid).frames >= after + 10; }, "no decoded video: " + sid);
  auto v = peer.observation(sid);
  require(v.width == 320 && v.height == 180 && std::abs(v.y - luma) <= 8, "wrong decoded camera pixels");
}
static void passed(Json& report, const std::string& id, Json evidence = Json::object()) {
  report["checks"].push_back({{"id", id}, {"status", "PASS"}, {"evidence", evidence}});
  std::cerr << "PASS " << id << '\n';
}
static void video_case(const std::string& url, const std::string& codec,
                       const std::filesystem::path& out, Json& report) {
  Peer drone(url, "drone", codec, out), a(url, "viewer-a", codec, out), b(url, "viewer-b", codec, out);
  VideoInput first(80), second(160);
  auto t1 = first.publish(drone, "device-01/camera-front/epoch-1");
  auto t2 = second.publish(drone, "device-01/camera-rear/epoch-1");
  for (auto* viewer : {&a, &b}) {
    verify_video(*viewer, t1.sid(), 80); verify_video(*viewer, t2.sid(), 160);
    for (const auto& t : {t1, t2}) {
      auto remote = viewer->remote("drone", t.sid());
      until([&] { return !remote.info().mime_type.empty(); }, "missing negotiated MIME");
      auto mime = remote.info().mime_type;
      std::transform(mime.begin(), mime.end(), mime.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      require(mime == "video/" + codec,
              "negotiated codec differs from requested codec: " + remote.info().mime_type);
    }
  }
  require(drone.room.local_participant().track_publications().size() == 2, "source duplicated per viewer");
  passed(report, "two_sources_two_viewers", {{"sources", 2}, {"subscriptions", 4}, {"codec", codec}, {"decoded_luma", {80, 160}}});
  auto selected = a.remote("drone", t1.sid()); done(selected.set_subscribed(false));
  until([&] { return !selected.info().subscribed; }, "unsubscribe not acknowledged");
  std::this_thread::sleep_for(500ms);
  auto stopped = a.observation(t1.sid()).frames;
  auto other = a.observation(t2.sid()).frames, independent = b.observation(t1.sid()).frames;
  verify_video(a, t2.sid(), 160, other); verify_video(b, t1.sid(), 80, independent);
  std::this_thread::sleep_for(700ms);
  require(a.observation(t1.sid()).frames == stopped, "unsubscribed video still decoding");
  done(selected.set_subscribed(true)); verify_video(a, t1.sid(), 80, stopped);
  passed(report, "independent_unsubscribe_resubscribe");
  // A stopped camera must not stop the second camera or another viewer.
  done(drone.room.local_participant().unpublish_track(t1.sid()));
  until([&] { return !first.source->stats().attached; }, "source not released");
  other = b.observation(t2.sid()).frames; verify_video(b, t2.sid(), 160, other);
  std::this_thread::sleep_for(500ms);
  stopped = a.observation(t1.sid()).frames;
  std::this_thread::sleep_for(700ms);
  require(a.observation(t1.sid()).frames == stopped, "old source still decoding");
  passed(report, "unpublish_isolation");
  auto renewed = first.publish(drone, "device-01/camera-front/epoch-2");
  require(renewed.sid() != t1.sid(), "republish reused server SID");
  verify_video(a, renewed.sid(), 80); verify_video(b, renewed.sid(), 80);
  passed(report, "republish_new_sid", {{"old_sid", t1.sid()}, {"new_sid", renewed.sid()}});
}
static void audio_case(const std::string& url, const std::filesystem::path& out, Json& report) {
  Peer drone(url, "drone", "h264", out), a(url, "viewer-a", "h264", out), b(url, "viewer-b", "h264", out);
  std::array<Peer*, 3> peers{&drone, &a, &b};
  AudioInput s0(440), s1(660), s2(880);
  std::array<TrackPublication, 3> tracks{s0.publish(drone, "drone-440"), s1.publish(a, "a-660"), s2.publish(b, "b-880")};
  const std::array<int, 3> frequencies{440, 660, 880};
  std::array<std::map<std::string, Tone>, 3> tones;
  const auto deadline = Clock::now() + 18s;
  bool complete = false;
  while (!complete) {
    require(Clock::now() < deadline, "six directed audio links did not decode");
    for (size_t i = 0; i < peers.size(); ++i) {
      auto r = peers[i]->audio->read(10ms); if (r.status != AudioReadStatus::frame) continue;
      require(r.frame->track_id != tracks[i].sid(), "self audio unexpectedly subscribed");
      auto& tone = tones[i][r.frame->track_id];
      // Ignore initial decoder silence, then count all decoded samples.
      if (!tone.count && std::none_of(r.frame->samples.begin(), r.frame->samples.end(), [](auto s) { return std::abs(int(s)) > 500; })) continue;
      tone.add(*r.frame);
    }
    complete = true;
    for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j)
      if (i != j && tones[i][tracks[j].sid()].count < 48000) complete = false;
  }
  Json links = Json::array();
  for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j) if (i != j) {
    const auto& t = tones[i][tracks[j].sid()];
    require(t.peak > 500 && std::abs(t.hz() - frequencies[j]) < frequencies[j] * .06, "wrong audio source/frequency");
    links.push_back({{"from", j}, {"to", i}, {"expected_hz", frequencies[j]}, {"decoded_hz", t.hz()}, {"samples", t.count}});
  }
  passed(report, "three_way_audio_six_links", links);
  // Leaving voice must preserve this member's video and remaining audio links.
  VideoInput camera(80); auto video = camera.publish(b, "viewer-b-video"); verify_video(a, video.sid(), 80);
  done(b.room.local_participant().unpublish_track(tracks[2].sid()));
  until([&] { return !s2.source->stats().attached; }, "leaving voice did not release PCM source");
  require(b.room.connection_state() == ConnectionState::connected, "voice leave disconnected member");
  auto before = a.observation(video.sid()).frames; verify_video(a, video.sid(), 80, before);
  std::this_thread::sleep_for(500ms);
  while (a.audio->read(0ms).status == AudioReadStatus::frame) {}
  while (drone.audio->read(0ms).status == AudioReadStatus::frame) {}
  std::array<int, 2> remaining{}; const auto end = Clock::now() + 1s;
  while (Clock::now() < end) {
    for (size_t i = 0; i < 2; ++i) {
      auto r = peers[i]->audio->read(10ms);
      if (r.status == AudioReadStatus::frame) {
        require(r.frame->track_id != tracks[2].sid(), "removed audio still decoding"); ++remaining[i];
      }
    }
  }
  require(remaining[0] > 20 && remaining[1] > 20, "remaining voice stalled in one direction");
  passed(report, "voice_leave_preserves_video_and_other_voice", {{"remaining_audio_blocks", remaining}});
}

int main(int argc, char** argv) {
  if (argc != 5) { std::cerr << "Usage: livekit_uav_requirements URL video|audio CODEC OUTPUT_DIR\n"; return 2; }
  const std::filesystem::path out(argv[4]); std::filesystem::create_directories(out);
  Json report{{"ok", false}, {"checks", Json::array()}, {"scenario", argv[2]}, {"codec", argv[3]}, {"device_acceptance", false}};
  try {
    if (std::string(argv[2]) == "video") video_case(argv[1], argv[3], out, report);
    else if (std::string(argv[2]) == "audio") audio_case(argv[1], out, report);
    else throw std::runtime_error("unknown scenario");
    report["ok"] = true;
  } catch (const std::exception& e) { report["error"] = e.what(); std::cerr << e.what() << '\n'; }
  std::ofstream(out / "client-result.json") << report.dump(2);
  return report["ok"].get<bool>() ? 0 : 1;
}
