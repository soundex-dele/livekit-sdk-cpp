#include "session.hpp"
#include "connection.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <atomic>
#include <algorithm>

using namespace std::chrono_literals;
namespace sdk = livekit::sdk;
static void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void until(F fn, const char* text) {
  const auto end = std::chrono::steady_clock::now() + 20s;
  while (!fn()) { check(std::chrono::steady_clock::now() < end, text); std::this_thread::sleep_for(30ms); }
}
static std::set<std::string> tracks(const demo::Snapshot& s) {
  std::set<std::string> result;
  if (auto p = s.participants.find(s.local_sid); p != s.participants.end())
    for (const auto& t : p->second.tracks) if (t.kind == sdk::TrackKind::video) result.insert(t.sid);
  return result;
}
static double video_bytes(const demo::Snapshot& s) {
  double total = 0; bool found = false;
  for (const auto& [id, stream] : s.quality.peers[0].streams)
    if (stream.sending && stream.kind == "video" && stream.bytes) { total += *stream.bytes; found = true; }
  check(found && total > 0 && !s.quality.peers[0].stale(), "Missing retained outbound RTP statistics");
  return total;
}
static double graph_frames(const demo::Snapshot& s) {
  double total = 0; size_t count = 0;
  for (const auto& [id, node] : s.quality.nodes)
    if ((node.stage == demo::QualitySnapshot::Stage::capture || node.stage == demo::QualitySnapshot::Stage::encode) && node.frames) {
      total += *node.frames; ++count;
    }
  check(count > 0 && !s.quality.graph_sample.stale(), "Missing source/encoder graph counters");
  return total;
}
int main(int argc, char** argv) {
  if (argc != 6) return 2;
  const std::filesystem::path out(argv[4]); std::filesystem::create_directories(out);
  sdk::Json report{{"ok", false}, {"checks", sdk::Json::array()}};
  try {
    std::ofstream events(out / "demo-publisher.jsonl");
    auto options = [&](const std::string& identity) {
      auto o = demo::username_options(argv[1], "uav-requirements", identity);
      o.media_transport = std::string(argv[5]) == "tcp" ? sdk::MediaTransport::tcp_only : sdk::MediaTransport::udp_only;
      return o;
    };
    demo::Session publisher;
    auto pub = options("drone"); sdk::MediaSpec video; video.demo = true; pub.media.push_back(video);
    pub.log_sink = [&](const sdk::Json& e) { const auto type = e.value("event", "");
      if (type == "source-demand" || type == "rtc-stats" || type == "media-graph-stats") events << e.dump() << '\n'; };
    publisher.start(pub, true);
    until([&] { return tracks(publisher.snapshot()).size() == 1; }, "Initial demo publication missing");
    const auto first = tracks(publisher.snapshot());
    auto paused = [&] {
      until([&] { const auto s = publisher.snapshot(); const auto ids = tracks(s);
        return !ids.empty() && std::all_of(ids.begin(), ids.end(), [&](const auto& sid) {
          auto it = s.video_demand.find(sid); return it != s.video_demand.end() && !it->second; });
      }, "Demo did not display aggregate OFF for every track");
      const auto initial = publisher.snapshot().quality.peers[0].timestamp_us;
      until([&] { return publisher.snapshot().quality.peers[0].timestamp_us >= initial + 2000000; }, "No paused samples");
      const auto a = publisher.snapshot(); const auto bytes = video_bytes(a), frames = graph_frames(a);
      until([&] { return publisher.snapshot().quality.peers[0].timestamp_us >= a.quality.peers[0].timestamp_us + 2000000; }, "No second paused sample");
      const auto b = publisher.snapshot();
      check(video_bytes(b) == bytes && graph_frames(b) == frames, "No viewers but source/encoder/RTP still growing");
    };
    paused();
    check(publisher.add_video(), "Dynamic video command rejected");
    until([&] { return tracks(publisher.snapshot()).size() == 2; }, "Dynamic video missing");
    paused(); // Dynamic Add test video must inherit the same policy.
    const auto published = tracks(publisher.snapshot());
    std::mutex mutex; std::map<std::string, uint64_t> a_frames, b_frames;
    sdk::Room a, b;
    auto ao = options("viewer-a"), bo = options("viewer-b");
    ao.video_frame_handler = [&](const sdk::VideoFrame& f) { std::lock_guard lock(mutex); ++a_frames[f.track_id]; };
    bo.video_frame_handler = [&](const sdk::VideoFrame& f) { std::lock_guard lock(mutex); ++b_frames[f.track_id]; };
    auto decoded = [&](const auto& counters) { return std::all_of(published.begin(), published.end(), [&](const auto& sid) {
      auto it = counters.find(sid); return it != counters.end() && it->second >= 10; }); };
    a.join_with_token(ao.url, ao.token, ao);
    until([&] { std::lock_guard lock(mutex); return decoded(a_frames); }, "First viewer did not decode both sources");
    b.join_with_token(bo.url, bo.token, bo);
    until([&] { std::lock_guard lock(mutex); return decoded(b_frames); }, "Second viewer did not decode both sources");
    a.disconnect(); a.wait();
    std::this_thread::sleep_for(6s);
    { std::lock_guard lock(mutex); b_frames.clear(); }
    until([&] { std::lock_guard lock(mutex); return decoded(b_frames); }, "Remaining viewer was stopped");
    for (const auto& sid : published) check(publisher.snapshot().video_demand.at(sid), "Remaining viewer demand OFF");
    b.disconnect(); b.wait(); paused();
    { std::lock_guard lock(mutex); a_frames.clear(); }
    a.join_with_token(ao.url, ao.token, ao);
    until([&] { std::lock_guard lock(mutex); return decoded(a_frames); }, "Video failed to resume");
    check(tracks(publisher.snapshot()) == published, "Resume republished instead of retaining SID");
    a.disconnect(); a.wait(); publisher.stop();
    until([&] { return publisher.snapshot().finished; }, "Demo stop timed out");
    check(publisher.snapshot().video_demand.empty(), "Demand state retained after leave");
    report["ok"] = true;
    report["checks"].push_back({{"id", "demo_aggregate_demand"}, {"status", "PASS"},
      {"evidence", {{"initial_and_dynamic_tracks", 2}, {"same_sids", published}, {"remaining_viewer_seconds", 6}, {"source_encoder_and_rtp_paused", true}}}});
  } catch (const std::exception& e) { report["error"] = e.what(); std::cerr << e.what() << '\n'; }
  std::ofstream(out / "client-result.json") << report.dump(2);
  std::cout << report.dump() << '\n';
  return report["ok"].get<bool>() ? 0 : 1;
}
