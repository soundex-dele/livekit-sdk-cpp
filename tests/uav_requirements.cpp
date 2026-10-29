// Headless C++ clients for the Go SFU requirement assessment. No capture devices.
#include <livekit/livekit.hpp>
#include "rtc_base/logging.h"
#include "api/peer_connection_interface.h"
#include <cstdlib>
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
static MediaTransport test_transport = MediaTransport::automatic;
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
#include "rtc_uav_peer.hpp"

struct VideoInput {
  std::shared_ptr<I420VideoSource> source = std::make_shared<I420VideoSource>(I420SourceOptions{320, 180});
  std::jthread writer;
  explicit VideoInput(int luma) : writer([this, luma](std::stop_token stop) {
    VideoFrame f; f.width = 320; f.height = 180; f.stride_y = 320; f.stride_u = f.stride_v = 160;
    f.y.assign(320 * 180, luma); f.u.assign(160 * 90, 100); f.v.assign(160 * 90, 160);
    while (!stop.stop_requested()) { source->write(f); std::this_thread::sleep_for(33ms); }
  }) {}
  ~VideoInput() {
    writer.request_stop(); writer.join(); const auto s = source->stats();
    std::cerr << Json{{"video_source_accepted", s.accepted}, {"consumed", s.consumed}, {"dropped", s.dropped}, {"accepting", s.accepting}}.dump() << '\n';
    source->close();
  }
  TestTrack publish(Peer& p, const std::string& name, bool dynacast = false) {
    MediaSpec m; m.width = 320; m.height = 180; m.video_source = source; m.dynacast = dynacast;
    return done(p.publish(m, name, transport::PublicationSource::camera));
  }
};
struct AudioInput {
  std::shared_ptr<PcmAudioSource> source = std::make_shared<PcmAudioSource>();
  std::jthread writer;
  explicit AudioInput(int hz) : writer([this, hz](std::stop_token stop) {
    uint64_t offset = 0;
    while (!stop.stop_requested()) {
      // Two 10 ms blocks per write: Windows may round sleep_for(2ms) to
      // a 15.6 ms tick, so a single block would underfeed a 48 kHz source.
      PcmInputFrame f; f.samples.resize(960);
      for (size_t i = 0; i < f.samples.size(); ++i)
        f.samples[i] = static_cast<int16_t>(6000 * std::sin(2 * std::numbers::pi * hz * (offset + i) / 48000));
      if (source->write(f) == AudioWriteStatus::accepted) offset += f.samples.size();
      std::this_thread::sleep_for(2ms);
    }
  }) {}
  ~AudioInput() {
    writer.request_stop(); writer.join(); const auto s = source->stats();
    std::cerr << Json{{"audio_source_accepted", s.accepted_samples}, {"consumed", s.consumed_samples},
      {"silence", s.silence_samples}, {"skipped", s.skipped_samples}, {"dropped", s.dropped_samples}}.dump() << '\n';
    source->close();
  }
  TestTrack publish(Peer& p, const std::string& name) {
    MediaSpec m; m.video = false; m.audio_source = source;
    return done(p.publish(m, name, transport::PublicationSource::microphone));
  }
};
struct Tone {
  uint64_t count = 0, crossings = 0; int previous = 0, rate = 0, peak = 0;
  uint64_t next_sample = 0, discontinuities = 0;
  std::vector<int16_t> pcm;
  void add(const AudioFrame& f) {
    require(f.format == AudioSampleFormat::pcm16 && f.channels > 0, "invalid PCM");
    if (rate && rate != f.sample_rate) { count = crossings = 0; previous = peak = 0; pcm.clear(); }
    if (count && f.sample_index != next_sample) ++discontinuities;
    next_sample = f.sample_index + f.samples_per_channel;
    rate = f.sample_rate;
    for (size_t i = 0; i < f.samples_per_channel; ++i) {
      const int sample = f.samples.at(i * f.channels);
      if (previous < 0 && sample >= 0) ++crossings;
      previous = sample; peak = std::max(peak, std::abs(sample)); ++count;
      if (pcm.size() < 192000) pcm.push_back(static_cast<int16_t>(sample));
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
    verify_video(*viewer, t1.sid, 80); verify_video(*viewer, t2.sid, 160);
    for (const auto& t : {t1, t2}) {
      auto remote = viewer->remote("drone", t.sid);
      until([&] { return !remote.mime_type.empty(); }, "missing negotiated MIME");
      auto mime = remote.mime_type;
      std::transform(mime.begin(), mime.end(), mime.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      require(mime == "video/" + codec,
              "negotiated codec differs from requested codec: " + remote.mime_type);
    }
  }
  require(drone.publications.load() == 2, "source duplicated per viewer");
  passed(report, "two_sources_two_viewers", {{"sources", 2}, {"subscriptions", 4}, {"codec", codec}, {"decoded_luma", {80, 160}}});
  auto selected = a.remote("drone", t1.sid); done(a.set_subscribed(selected.id, false));
  until([&] { return !a.subscribed(selected.id); }, "unsubscribe not acknowledged");
  std::this_thread::sleep_for(500ms);
  auto stopped = a.observation(t1.sid).frames;
  auto other = a.observation(t2.sid).frames, independent = b.observation(t1.sid).frames;
  verify_video(a, t2.sid, 160, other); verify_video(b, t1.sid, 80, independent);
  std::this_thread::sleep_for(700ms);
  require(a.observation(t1.sid).frames == stopped, "unsubscribed video still decoding");
  done(a.set_subscribed(selected.id, true)); verify_video(a, t1.sid, 80, stopped);
  passed(report, "independent_unsubscribe_resubscribe");
  // A stopped camera must not stop the second camera or another viewer.
  done(drone.unpublish(t1.id));
  until([&] { return !first.source->stats().attached; }, "source not released");
  other = b.observation(t2.sid).frames; verify_video(b, t2.sid, 160, other);
  std::this_thread::sleep_for(500ms);
  stopped = a.observation(t1.sid).frames;
  std::this_thread::sleep_for(700ms);
  require(a.observation(t1.sid).frames == stopped, "old source still decoding");
  passed(report, "unpublish_isolation");
  auto renewed = first.publish(drone, "device-01/camera-front/epoch-2");
  require(renewed.sid != t1.sid, "republish reused server SID");
  verify_video(a, renewed.sid, 80); verify_video(b, renewed.sid, 80);
  passed(report, "republish_new_sid", {{"old_sid", t1.sid}, {"new_sid", renewed.sid}});
}
static void audio_case(const std::string& url, const std::string& codec,
                       const std::filesystem::path& out, Json& report, bool mixed = false) {
  Peer drone(url, "drone", codec, out), a(url, "viewer-a", codec, out), b(url, "viewer-b", codec, out);
  std::unique_ptr<VideoInput> front, rear;
  std::optional<TestTrack> front_track, rear_track;
  if (mixed) {
    front = std::make_unique<VideoInput>(80); rear = std::make_unique<VideoInput>(160);
    front_track = front->publish(drone, "mixed-front"); rear_track = rear->publish(drone, "mixed-rear");
    for (auto* viewer : {&a, &b}) {
      verify_video(*viewer, front_track->sid, 80); verify_video(*viewer, rear_track->sid, 160);
    }
    passed(report, "two_sources_two_viewers", {{"codec", codec}, {"sources", 2}, {"subscriptions", 4}});
  }
  std::array<Peer*, 3> peers{&drone, &a, &b};
  AudioInput s0(440), s1(660), s2(880);
  std::array<TestTrack, 3> tracks{s0.publish(drone, "drone-440"), s1.publish(a, "a-660"), s2.publish(b, "b-880")};
  const std::array<int, 3> frequencies{440, 660, 880};
  std::array<std::map<std::string, Tone>, 3> tones;
  const auto deadline = Clock::now() + 18s;
  bool complete = false;
  while (!complete) {
    require(Clock::now() < deadline, "six directed audio links did not decode");
    for (size_t i = 0; i < peers.size(); ++i) {
      auto r = peers[i]->read_audio(10ms); if (!r.frame) continue;
      require(r.frame->track_id != tracks[i].sid, "self audio unexpectedly subscribed");
      auto& tone = tones[i][r.frame->track_id];
      // Ignore initial decoder silence, then count all decoded samples.
      if (!tone.count && std::none_of(r.frame->samples.begin(), r.frame->samples.end(), [](auto s) { return std::abs(int(s)) > 500; })) continue;
      tone.add(*r.frame);
    }
    complete = true;
    for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j)
      if (i != j && tones[i][tracks[j].sid].count < 48000) complete = false;
  }
  Json links = Json::array();
  for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j) if (i != j) {
    const auto& t = tones[i][tracks[j].sid];
    std::ofstream pcm(out / ("audio-" + std::to_string(j) + "-to-" + std::to_string(i) + ".s16le"), std::ios::binary);
    pcm.write(reinterpret_cast<const char*>(t.pcm.data()), t.pcm.size() * sizeof(int16_t));
    std::cerr << Json{{"from", j}, {"to", i}, {"expected_hz", frequencies[j]}, {"decoded_hz", t.hz()},
      {"samples", t.count}, {"peak", t.peak}, {"sample_rate", t.rate}, {"discontinuities", t.discontinuities}}.dump() << '\n';
  }
  for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j) if (i != j) {
    const auto& t = tones[i][tracks[j].sid];
    require(t.peak > 500 && std::abs(t.hz() - frequencies[j]) < frequencies[j] * .06, "wrong audio source/frequency");
    links.push_back({{"from", j}, {"to", i}, {"expected_hz", frequencies[j]}, {"decoded_hz", t.hz()}, {"samples", t.count}});
  }
  passed(report, "three_way_audio_six_links", links);
  // Leaving voice must preserve this member's video and remaining audio links.
  VideoInput camera(80); auto video = camera.publish(b, "viewer-b-video"); verify_video(a, video.sid, 80);
  done(b.unpublish(tracks[2].id));
  until([&] { return !s2.source->stats().attached; }, "leaving voice did not release PCM source");
  require(b.connected.load(), "voice leave disconnected member");
  auto before = a.observation(video.sid).frames; verify_video(a, video.sid, 80, before);
  std::this_thread::sleep_for(500ms);
  while (a.read_audio(0ms).frame) {}
  while (drone.read_audio(0ms).frame) {}
  std::array<int, 2> remaining{}; const auto end = Clock::now() + 1s;
  while (Clock::now() < end) {
    for (size_t i = 0; i < 2; ++i) {
      auto r = peers[i]->read_audio(10ms);
      if (bool(r.frame)) {
        require(r.frame->track_id != tracks[2].sid, "removed audio still decoding"); ++remaining[i];
      }
    }
  }
  require(remaining[0] > 20 && remaining[1] > 20, "remaining voice stalled in one direction");
  passed(report, "voice_leave_preserves_video_and_other_voice", {{"remaining_audio_blocks", remaining}});
  if (mixed) {
    for (auto* viewer : {&a, &b}) {
      verify_video(*viewer, front_track->sid, 80, viewer->observation(front_track->sid).frames);
      verify_video(*viewer, rear_track->sid, 160, viewer->observation(rear_track->sid).frames);
    }
    passed(report, "video_continues_after_concurrent_audio_and_voice_leave");
  }
}

static void dynacast_case(const std::string& url, const std::string& codec,
                          const std::filesystem::path& out, Json& report) {
  Peer drone(url, "drone", codec, out);
  VideoInput source(80); auto track = source.publish(drone, "demand-camera", true);
  auto stopped = [&] {
    until([&] { return drone.has_demand(track.sid, false) && !source.source->stats().accepting; }, "no aggregate OFF at publisher");
    const auto samples = drone.stats_samples.load();
    until([&] { return drone.stats_samples.load() >= samples + 2; }, "missing publisher stats after OFF");
    const auto consumed = source.source->stats().consumed, bytes = drone.video_bytes.load();
    require(drone.video_stats_rows.load() > 0 && bytes > 0, "missing retained outbound RTP statistics");
    const auto stable = drone.stats_samples.load();
    until([&] { return drone.stats_samples.load() >= stable + 2; }, "missing paused publisher stats");
    require(source.source->stats().attached && !source.source->stats().accepting, "paused source lost ownership");
    require(source.source->stats().consumed == consumed && drone.video_stats_rows.load() > 0 && drone.video_bytes.load() == bytes,
      "OFF did not stop source/actual video RTP");
    return Json{{"consumed", consumed}, {"video_bytes", bytes}, {"stable_stats_intervals", 2}};
  };
  auto initial = stopped();
  Peer a(url, "viewer-a", codec, out);
  verify_video(a, track.sid, 80);
  Peer b(url, "viewer-b", codec, out);
  verify_video(b, track.sid, 80);
  auto ta = a.remote("drone", track.sid), tb = b.remote("drone", track.sid);
  done(a.set_subscribed(ta.id, false));
  // Outlast the server's default 5 s pause debounce. Otherwise a broken
  // per-viewer OFF decision could be hidden by immediately removing viewer B.
  std::this_thread::sleep_for(6s);
  verify_video(b, track.sid, 80, b.observation(track.sid).frames);
  require(source.source->stats().accepting, "one viewer leaving stopped shared source");
  done(b.set_subscribed(tb.id, false));
  auto empty = stopped();
  const auto before = a.observation(track.sid).frames;
  done(a.set_subscribed(ta.id, true)); verify_video(a, track.sid, 80, before);
  until([&] { return drone.has_demand(track.sid, true); }, "missing aggregate ON");
  require(drone.publications.load() == 1, "dynacast republished source instead of resuming");
  passed(report, "aggregate_demand_stops_source_and_rtp", {{"initial_zero_viewers", initial}, {"last_viewer_left", empty},
    {"resumed_same_sid", track.sid}, {"single_remaining_viewer_seconds", 6}, {"codec", codec}});
}

static void transport_reject_case(const std::string& url, Json& report) {
  auto room = transport::create_livekit_room();
  transport::RoomOptions options; options.url = url; options.token = token("drone");
  options.media_transport = test_transport; options.connect_timeout = 4; options.reconnect.enabled = false;
  require(test_transport != MediaTransport::automatic, "negative test requires strict transport");
  bool joined = false, connected = false; const auto start = Clock::now();
  auto joining = room->join(options);
  while (joining.wait_for(0ms) != std::future_status::ready) {
    if (auto event = room->next_event(10ms)) {
      joined |= event->kind == transport::RoomEventKind::session_changed;
      connected |= event->kind == transport::RoomEventKind::connected;
    }
  }
  try { joining.get(); }
  catch (const SDKError& error) {
    require(joined && !connected && (error.code() == ErrorCode::timeout || error.code() == ErrorCode::connection_lost),
            "failure was not an expected media transport rejection");
    passed(report, "opposite_protocol_rejected", {{"signaling_joined", joined}, {"error", error.what()},
      {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()}});
    return;
  }
  throw std::runtime_error("opposite protocol unexpectedly connected");
}

int main(int argc, char** argv) {
  if (std::getenv("LIVEKIT_UAV_NATIVE_LOG")) {
    webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
    webrtc::LogMessage::SetLogToStderr(true);
  }
  if (argc != 5 && argc != 6) { std::cerr << "Usage: livekit_uav_requirements URL video|audio|dynacast|transport-reject CODEC OUTPUT_DIR [auto|udp|tcp]\n"; return 2; }
  const std::filesystem::path out(argv[4]); std::filesystem::create_directories(out);
  Json report{{"ok", false}, {"checks", Json::array()}, {"scenario", argv[2]}, {"codec", argv[3]}, {"device_acceptance", false}};
  try {
    const std::string transport = argc == 6 ? argv[5] : "auto";
    if (transport == "udp") test_transport = MediaTransport::udp_only;
    else if (transport == "tcp") test_transport = MediaTransport::tcp_only;
    else require(transport == "auto", "unknown media transport");
    report["client_transport"] = transport;
    if (std::string(argv[2]) == "video") video_case(argv[1], argv[3], out, report);
    else if (std::string(argv[2]) == "audio") audio_case(argv[1], argv[3], out, report);
    else if (std::string(argv[2]) == "mixed") audio_case(argv[1], argv[3], out, report, true);
    else if (std::string(argv[2]) == "dynacast") dynacast_case(argv[1], argv[3], out, report);
    else if (std::string(argv[2]) == "transport-reject") transport_reject_case(argv[1], report);
    else throw std::runtime_error("unknown scenario");
    report["ok"] = true;
  } catch (const std::exception& e) { report["error"] = e.what(); std::cerr << e.what() << '\n'; }
  std::ofstream(out / "client-result.json") << report.dump(2);
  return report["ok"].get<bool>() ? 0 : 1;
}
