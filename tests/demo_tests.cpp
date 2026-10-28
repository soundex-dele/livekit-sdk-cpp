#define SDL_MAIN_HANDLED
#include "ui.hpp"
#include "connection.hpp"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include <filesystem>
#include <iostream>
#include <cmath>

static void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static demo::sdk::Json quality_report(int target, double time, double bytes) {
  using demo::sdk::Json;
  return Json{{"target", target}, {"timestampUs", time}, {"stats", Json::array({
    {{"id", "transport"}, {"type", "transport"}, {"selectedCandidatePairId", "pair"}},
    {{"id", "pair"}, {"type", "candidate-pair"}, {"currentRoundTripTime", .025}},
    {{"id", "unused-pair"}, {"type", "candidate-pair"}, {"currentRoundTripTime", 9.0}},
    {{"id", "rtp"}, {"type", target == 0 ? "outbound-rtp" : "inbound-rtp"},
     {"ssrc", 1234}, {"kind", "video"}, {target == 0 ? "bytesSent" : "bytesReceived", bytes},
     {target == 0 ? "retransmittedBytesSent" : "retransmittedBytesReceived", bytes / 10},
     {"fecBytesReceived", bytes / 50},
     {"framesPerSecond", 99}, {target == 0 ? "framesSent" : "framesReceived", time / 1e6 * (target == 0 ? 28 : 27)},
     {"packetsLost", 4}, {"jitter", .003}, {"mediaSourceId", "src"}, {"trackIdentifier", "TR_two"}},
    {{"id", "src"}, {"type", "media-source"}, {"trackIdentifier", "video-1"}}
  })}};
}
static void test_repair_rates() {
  demo::QualitySnapshot q;
  for (int target : {0, 1}) {
    q.update(quality_report(target, 1000000, 1000));
    check(!q.peers[target].streams.at("rtp").retransmit_kbps && !q.peers[target].streams.at("rtp").fec_kbps,
          "Repair rates require two samples");
    q.update(quality_report(target, 2000000, 101000));
    const auto& stream = q.peers[target].streams.at("rtp");
    check(stream.retransmit_kbps == 80, "Send/receive retransmit payload rate");
    check(target == 0 ? !stream.fec_kbps : stream.fec_kbps == 16, "Only receive FEC bytes are supported");
  }
  q.update(quality_report(1, 3000000, 101000));
  check(q.peers[1].streams.at("rtp").retransmit_kbps == 0 && q.peers[1].streams.at("rtp").fec_kbps == 0,
        "Measured zero repair rate must remain available");
  auto row = quality_report(1, 4000000, 201000);
  row["stats"][3]["retransmittedBytesReceived"] = 1;
  row["stats"][3]["fecBytesReceived"] = 1;
  q.update(row);
  check(!q.peers[1].streams.at("rtp").retransmit_kbps && !q.peers[1].streams.at("rtp").fec_kbps &&
        q.peers[1].streams.at("rtp").kbps == 800, "Repair counter resets do not invalidate RTP rate");
  row = quality_report(1, 5000000, 301000);
  row["stats"][3].erase("retransmittedBytesReceived"); row["stats"][3].erase("fecBytesReceived");
  q.update(row);
  check(!q.peers[1].streams.at("rtp").retransmit_kbps && !q.peers[1].streams.at("rtp").fec_kbps,
        "Absent repair counters must not appear as zero");
  q.update(quality_report(1, 6000000, 401000));
  check(!q.peers[1].streams.at("rtp").fec_kbps, "Restored counter needs a new baseline");
  q.update(quality_report(1, 10000000, 501000));
  check(!q.peers[1].streams.at("rtp").retransmit_kbps && !q.peers[1].streams.at("rtp").fec_kbps,
        "Long gap invalidates repair rates");
  row = quality_report(1, 11000000, 601000); row["stats"][3]["ssrc"] = 5678;
  q.update(row);
  check(!q.peers[1].streams.at("rtp").retransmit_kbps && !q.peers[1].streams.at("rtp").fec_kbps,
        "New SSRC cannot inherit repair rates");
  row = quality_report(1, 12000000, 701000); row["stats"][3]["ssrc"] = 5678;
  row["stats"][3]["fecBytesReceived"] = "invalid";
  q.update(row);
  check(q.peers[1].streams.at("rtp").retransmit_kbps == 80 && !q.peers[1].streams.at("rtp").fec_kbps,
        "Malformed FEC counter does not hide valid retransmit rate");
  check(q.peers[0].streams.at("rtp").retransmit_kbps == 80, "Connections retain independent repair baselines");
}
static demo::QualitySnapshot test_quality() {
  demo::QualitySnapshot q;
  q.update(quality_report(1, 1000000, 1000));
  check(!q.peers[1].streams.at("rtp").kbps && !q.peers[1].streams.at("rtp").fps, "First sample is not a rate");
  q.update(quality_report(1, 2000000, 101000));
  const auto& stream = q.peers[1].streams.at("rtp");
  check(stream.kbps && std::abs(*stream.kbps - 800) < .01, "RTP delta kbps");
  check(stream.jitter_ms == 3 && stream.lost == 4 && !stream.fps, "Receive FPS must not use WebRTC's bypassed decoder counters");
  check(q.peers[1].rtt_ms == 25, "RTT must come from selected candidate pair");
  q.update(quality_report(0, 1000000, 500));
  check(!q.peers[0].streams.at("rtp").kbps && stream.kbps == 800, "Independent peer baselines");
  q.update(quality_report(0, 2000000, 50500));
  check(q.peers[0].streams.at("rtp").kbps == 400 && !q.peers[0].streams.at("rtp").lost, "Send rate and unavailable loss");
  using Stage = demo::QualitySnapshot::Stage;
  check(q.fps(Stage::send) == 28 && !q.fps(Stage::receive), "Send counter and missing graph receive counter");
  auto graph = [](double time, double count, unsigned generation = 1) {
    using demo::sdk::Json;
    return Json{{"event", "media-graph-stats"}, {"timestampUs", time}, {"nodes", Json::array({
      {{"id", 1u}, {"name", "source"}, {"trackId", "TR_local1"}, {"kind", "VideoSourceNode"}, {"generation", generation}, {"outputs", count * 30}},
      {{"id", 2u}, {"name", "encoder"}, {"trackId", "TR_local1"}, {"kind", "VideoEncoderNode"}, {"generation", generation}, {"outputs", count * 29}},
      {{"id", 3u}, {"name", "decoder"}, {"trackId", "TR_two"}, {"kind", "VideoDecoderNode"}, {"generation", generation}, {"outputs", count * 26}},
      {{"id", 4u}, {"name", "audio"}, {"kind", "AudioSourceNode"}, {"generation", generation}, {"outputs", count * 100}},
      {{"id", 5u}, {"name", "encoded-file"}, {"kind", "VideoSourceNode"}, {"generation", generation}, {"outputs", count * 60}},
      {{"id", 7u}, {"name", "receive"}, {"trackId", "TR_two"}, {"kind", "RtcEncodedSourceNode"}, {"generation", generation}, {"outputs", count * 27}}
    })}, {"localTracks", {{"video-1", "TR_local1"}}}, {"edges", Json::array({{{"from", "source"}, {"dataType", "RawVideoFrame"}},
                               {{"from", "encoded-file"}, {"dataType", "EncodedVideoFrame"}}})}};
  };
  q.update(graph(1000000, 1));
  check(!q.fps(Stage::capture) && !q.fps(Stage::encode) && !q.fps(Stage::decode), "Graph requires baseline");
  q.update(graph(2000000, 2));
  check(q.fps(Stage::capture, "TR_local1") == 30 && q.fps(Stage::encode, "TR_local1") == 29 &&
        q.fps(Stage::send, "TR_local1") == 28 && q.fps(Stage::receive, "TR_two") == 27 &&
        q.fps(Stage::decode, "TR_two") == 26 && q.nodes.size() == 4, "Five independent per-track FPS values");
  check(!q.fps(Stage::capture, "TR_two") && !q.fps(Stage::decode, "TR_local1") && !q.fps(Stage::send, "TR_unknown"),
        "Tracks must not borrow other tracks' counters");
  const auto fixture = q;
  auto two_senders = fixture;
  two_senders.local_tracks["video-2"] = "TR_local2";
  auto two_reports = quality_report(0, 3000000, 150500);
  auto second_rtp = two_reports["stats"][3];
  second_rtp["id"] = "rtp2"; second_rtp["ssrc"] = 5678; second_rtp["mediaSourceId"] = "src2"; second_rtp["framesSent"] = 90;
  two_reports["stats"].push_back(second_rtp);
  two_reports["stats"].push_back({{"id", "src2"}, {"type", "media-source"}, {"trackIdentifier", "video-2"}});
  two_senders.update(two_reports);
  check(two_senders.fps(Stage::send, "TR_local1") == 28 && !two_senders.fps(Stage::send, "TR_local2"),
        "Adding a sender only resets that track's baseline");
  two_reports["timestampUs"] = 4000000; two_reports["stats"][3]["framesSent"] = 112;
  two_reports["stats"][5]["framesSent"] = 105;
  two_senders.update(two_reports);
  check(two_senders.fps(Stage::send, "TR_local1") == 28 && two_senders.fps(Stage::send, "TR_local2") == 15,
        "Different sender SSRCs map to independent track rates");
  auto multi = graph(3000000, 3);
  auto second_source = multi["nodes"][0]; second_source["id"] = 6u; second_source["name"] = "source2"; second_source["trackId"] = "TR_local2";
  multi["nodes"].push_back(second_source);
  multi["edges"].push_back({{"from", "source2"}, {"dataType", "RawVideoFrame"}});
  auto multi_quality = fixture; multi_quality.update(multi);
  check(!multi_quality.fps(Stage::capture), "Partial multi-stream baseline must not undercount");
  multi["timestampUs"] = 4000000;
  multi["nodes"][0]["outputs"] = 120; multi["nodes"][6]["outputs"] = 105;
  multi_quality.update(multi);
  check(multi_quality.fps(Stage::capture, "TR_local1") == 30 && multi_quality.fps(Stage::capture, "TR_local2") == 15,
        "Different capture streams retain independent rates");
  auto rebound = multi; rebound["timestampUs"] = 5000000; rebound["nodes"][6]["trackId"] = "TR_new";
  multi_quality.update(rebound);
  check(!multi_quality.fps(Stage::capture, "TR_local2") && !multi_quality.fps(Stage::capture, "TR_new"),
        "Rebound track cannot inherit previous track baseline");
  auto transport_quality = fixture;
  auto frames_reset = quality_report(0, 3000000, 150500); frames_reset["stats"][3]["framesSent"] = 1;
  transport_quality.update(frames_reset);
  check(!transport_quality.fps(Stage::send) && transport_quality.peers[0].streams.at("rtp").kbps == 800,
        "Frame counter reset is independent of byte counter");
  frames_reset["timestampUs"] = 4000000; transport_quality.update(frames_reset);
  check(transport_quality.fps(Stage::send) == 0, "No sent frames means zero FPS");
  frames_reset["timestampUs"] = 5000000; frames_reset["stats"][3].erase("framesSent");
  transport_quality.update(frames_reset);
  check(!transport_quality.fps(Stage::send), "Missing frame counter cannot use native FPS as fallback");
  transport_quality.peers[0].updated -= std::chrono::seconds(4);
  check(!transport_quality.fps(Stage::send), "Stale RTP hides send FPS");
  q.update(graph(1500000, 9));
  check(q.fps(Stage::decode) == 26, "Graph ignores reordered report");
  q.update(graph(3000000, 2));
  check(q.fps(Stage::capture) == 0 && q.fps(Stage::encode) == 0 && q.fps(Stage::decode) == 0, "Stopped frames are measured zero");
  q.update(graph(4000000, 1));
  check(!q.fps(Stage::encode), "Graph counter reset invalidates rate");
  q.update(graph(5000000, 2, 2));
  check(!q.fps(Stage::decode), "Codec generation invalidates baseline even with increasing counters");
  q.update(graph(9000000, 6, 2));
  check(!q.fps(Stage::capture), "Graph gap invalidates rates");
  q.update(graph(10000000, 7, 2));
  q.graph_sample.updated -= std::chrono::seconds(4);
  check(!q.fps(Stage::capture) && !q.fps(Stage::encode) && !q.fps(Stage::decode), "Stale graph hides all stage rates");
  auto removed = graph(11000000, 8, 2); removed["nodes"] = demo::sdk::Json::array();
  q.update(removed);
  check(q.nodes.empty() && !q.fps(Stage::decode), "Removed nodes do not linger");
  q.update(quality_report(1, 1500000, 200000));
  check(q.peers[1].timestamp_us == 2000000, "Ignore old samples");
  q.update(quality_report(1, 3000000, 10));
  check(!q.peers[1].streams.at("rtp").kbps, "Counter reset must not produce negative rates");
  q.update(quality_report(1, 4000000, 10));
  check(q.peers[1].streams.at("rtp").kbps == 0, "Measured zero differs from missing data");
  q.update(quality_report(1, 9000000, 100000));
  check(!q.peers[1].streams.at("rtp").kbps, "Long gap resets rate baseline");
  auto missing = quality_report(1, 10000000, 200000);
  missing["stats"][3].erase("jitter"); missing["stats"][3].erase("framesReceived");
  missing["stats"][3]["ssrc"] = 5678;
  q.update(missing);
  check(!q.peers[1].streams.at("rtp").kbps && !q.peers[1].streams.at("rtp").jitter_ms &&
        !q.peers[1].streams.at("rtp").fps, "New SSRC and missing fields");
  q.peers[1].updated -= std::chrono::seconds(4);
  check(q.peers[1].stale(), "Stale report detection");
  q.update({{"target", 1}, {"timestampUs", 11000000}, {"stats", demo::sdk::Json::array()}});
  check(q.peers[1].streams.empty() && !q.peers[1].rtt_ms, "Removed stats must not linger");
  q.update({{"target", 8}, {"timestampUs", 12000000}, {"stats", demo::sdk::Json::array()}});
  q.update({{"target", 1}, {"timestampUs", "invalid"}});
  check(q.peers[1].timestamp_us == 11000000, "Malformed stats ignored");
  return fixture;
}
static void test_track_bitrates(demo::QualitySnapshot q) {
  using Rate = demo::QualitySnapshot::Bitrate;
  check(q.bitrate(Rate::rtp, "TR_local1", true) == 400 && q.bitrate(Rate::retransmit, "TR_local1", true) == 40,
        "Local media source must map to its Track SID");
  check(q.bitrate(Rate::rtp, "TR_two", false) == 800 && q.bitrate(Rate::fec, "TR_two", false) == 16,
        "Remote track bitrate mapping");
  check(!q.bitrate(Rate::rtp, "TR_unknown", false) && !q.bitrate(Rate::rtp, "TR_two", true) &&
        !q.bitrate(Rate::rtp, "", false), "Unknown track or wrong direction must not borrow a total");
  auto other = q.peers[1].streams.at("rtp");
  other.id = "second"; other.ssrc = "5678"; other.track_id = "TR_other";
  other.kbps = 200; other.retransmit_kbps = 5; other.fec_kbps.reset();
  q.peers[1].streams.emplace(other.id, other);
  check(q.bitrate(Rate::rtp, "TR_two", false) == 800 && q.bitrate(Rate::rtp, "TR_other", false) == 200 &&
        q.bitrate(Rate::fec, "TR_two", false) == 16 && !q.bitrate(Rate::fec, "TR_other", false),
        "Different tracks must retain distinct rates and availability");
  q.remote_tracks[1]["5678"] = "TR_two";
  check(q.bitrate(Rate::rtp, "TR_two", false) == 1000 && q.bitrate(Rate::retransmit, "TR_two", false) == 85 &&
        !q.bitrate(Rate::fec, "TR_two", false) && !q.bitrate(Rate::rtp, "TR_other", false),
        "SSRC mapping takes precedence and incomplete layer totals remain unavailable");
  q.peers[1].streams.at("second").kbps.reset();
  check(!q.bitrate(Rate::rtp, "TR_two", false) && q.bitrate(Rate::rtp, "TR_local1", true) == 400,
        "A new layer invalidates only its own track total");
  q.peers[1].streams.erase("second");
  check(q.bitrate(Rate::rtp, "TR_two", false) == 800, "Removed layer must not linger");
  q.peers[1].updated -= std::chrono::seconds(4);
  check(!q.bitrate(Rate::rtp, "TR_two", false) && !q.bitrate(Rate::retransmit, "TR_two", false) &&
        !q.bitrate(Rate::fec, "TR_two", false), "Stale per-track bitrates must be hidden");
}
static std::shared_ptr<demo::sdk::VideoFrame> pixels(const std::string& id, int w = 321, int h = 181) {
  auto f = std::make_shared<demo::sdk::VideoFrame>(); f->track_id = id; f->width = w; f->height = h;
  f->stride_y = w + 7; f->stride_u = (w + 1) / 2 + 9; f->stride_v = (w + 1) / 2 + 11;
  f->y.assign(f->stride_y * h, 81); f->u.assign(f->stride_u * ((h + 1) / 2), 90); f->v.assign(f->stride_v * ((h + 1) / 2), 240); return f;
}
int main(int argc, char** argv) {
  try {
    test_repair_rates();
    const auto quality_fixture = test_quality();
    test_track_bitrates(quality_fixture);
    SDL_SetMainReady(); check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL init");
    auto* window = SDL_CreateWindow("Render test", 0, 0, 1100, 720, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    check(window, "window"); auto* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE); check(renderer, "renderer");
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer); ImGui_ImplSDLRenderer2_Init(renderer); demo::apply_style(1);
    {
      demo::VideoTexture texture; auto frame = pixels("TR_one"); texture.upload(renderer, *frame);
      SDL_RenderCopy(renderer, texture.get(), nullptr, nullptr);
      std::vector<uint32_t> buffer(1100 * 720);
      check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, buffer.data(), 1100 * 4) == 0, "readback");
      auto center = buffer[360 * 1100 + 550];
      check(((center >> 16) & 255) > 220 && ((center >> 8) & 255) < 35 && (center & 255) < 35, "I420 red: planes/colors swapped");
      texture.upload(renderer, *pixels("TR_one", 181, 321)); check(texture.width() == 181, "resolution recreation");
      auto fit = demo::fit_video(0, 0, 500, 500, 640, 360);
      check(std::abs(fit.w - 500) < .01f && std::abs(fit.h - 281.25f) < .01f && fit.y > 100, "aspect ratio");
      auto invalid = *frame; invalid.v.clear(); bool rejected = false;
      try { texture.upload(renderer, invalid); } catch (...) { rejected = true; } check(rejected, "invalid buffer validation");
      demo::Snapshot state; state.state = demo::State::Connected; state.finished = false; state.room = "demo-room";
      demo::sdk::ParticipantInfo local; local.sid = "PA_me"; local.identity = "viewer";
      demo::sdk::ParticipantInfo alice; alice.sid = "PA_alice"; alice.identity = "alice"; alice.name = "Alice";
      demo::sdk::TrackInfo t; t.sid = "TR_one"; t.kind = demo::sdk::TrackKind::video; t.subscribed = true; alice.tracks.push_back(t);
      demo::sdk::ParticipantInfo bob = alice; bob.sid = "PA_bob"; bob.identity = "bob"; bob.name = "Bob"; bob.tracks[0].sid = "TR_two";
      demo::sdk::ParticipantInfo guest; guest.sid = "PA_guest"; guest.identity = "guest";
      state.frames[t.sid] = frame; // pixels may precede participant metadata
      state.update(local, {alice, bob, guest});
      check(demo::tiles(state).size() == 3, "roster tile count");
      state.frames["TR_two"] = pixels("TR_two");
      demo::View view;
      bool missing_username = false;
      try { view.options(); } catch (const std::invalid_argument&) { missing_username = true; }
      check(missing_username, "Blank username must be rejected");
      std::snprintf(view.username.data(), view.username.size(), " alice ");
      view.video_count = 3; view.tone = true;
      auto publishing = view.options();
      const auto first_dot = publishing.token.find('.'), last_dot = publishing.token.rfind('.');
      const auto claims = demo::sdk::Json::parse(demo::sdk::base64_decode(
        publishing.token.substr(first_dot + 1, last_dot - first_dot - 1)));
      check(claims.at("sub") == "alice" && claims.at("name") == "alice" &&
        claims.at("video").at("room") == "demo" && claims.at("video").at("roomJoin") == true &&
        claims.at("video").at("canPublish") == true && claims.at("video").at("canSubscribe") == true,
        "Username must produce a room-scoped publish/subscribe token");
      bool whitespace_rejected = false;
      try { demo::username_options(view.url.data(), " \t", "alice"); }
      catch (const std::invalid_argument&) { whitespace_rejected = true; }
      check(whitespace_rejected, "Whitespace room must be rejected");
      check(publishing.media.size() == 4 && publishing.media[0].video && publishing.media[2].video && !publishing.media[3].video, "Multiple videos and independent tone");
      view.video_count = -1; check(view.options().media.size() == 1, "Negative video count");
      view.video_count = 99; check(view.options().media.size() == demo::max_test_videos + 1, "Video count limit");
      view.video_count = 2; view.tone = false;
      auto extra = t; extra.sid = "TR_three"; extra.name = "Test video 2";
      alice.tracks[0].name = "Test video 1"; alice.tracks.push_back(extra);
      local.tracks = alice.tracks; local.tracks[0].sid = "TR_local1"; local.tracks[1].sid = "TR_local2";
      state.update(local, {alice, bob, guest}); state.frames[extra.sid] = pixels(extra.sid);
      state.pause_unused_video = true;
      state.video_demand = {{"TR_local1", true}, {"TR_local2", false}, {"removed", true}};
      state.update(local, {alice, bob, guest});
      check(!state.video_demand.contains("removed") && state.video_demand.size() == 2, "Removed demand state retained");
      check(demo::tiles(state).size() == 4 && demo::tiles(state)[0].label == "Alice / Test video 1" && demo::tiles(state)[1].label == "Alice / Test video 2", "Same participant video labels");
      float quality_scroll = -1;
      auto render = [&](demo::Snapshot snapshot) {
        ImGui_ImplSDLRenderer2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        view.draw(renderer, snapshot);
        if (view.show_quality && quality_scroll >= 0) {
          ImGui::Begin("Quality dashboard"); ImGui::SetScrollY(quality_scroll); ImGui::End();
        }
        ImGui::Render(); SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
        demo::render_draw_data(renderer);
      };
      render(state); render(state);
      check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, buffer.data(), 1100 * 4) == 0, "ImGui readback");
      size_t red = 0; for (auto c : buffer) if (((c >> 16) & 255) > 220 && ((c >> 8) & 255) < 35 && (c & 255) < 35) ++red;
      check(red > 30000, "ImGui did not render video textures");
      if (argc > 1) {
        auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(buffer.data(), 1100, 720, 32, 1100 * 4, SDL_PIXELFORMAT_ARGB8888);
        check(SDL_SaveBMP(surface, argv[1]) == 0, "save visual QA"); SDL_FreeSurface(surface);
      }
      // Emulate a Retina backing buffer without opening a physical window.
      auto* retina = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_TARGET, 2200, 1440);
      check(retina, "Retina target");
      check(SDL_SetRenderTarget(renderer, retina) == 0, "Retina render target");
      ImGui_ImplSDLRenderer2_DestroyFontsTexture(); demo::apply_style(1, 2);
      render(state); render(state);
      check(std::abs(ImGui::GetIO().DisplayFramebufferScale.x - 2) < .01f, "Retina framebuffer scale");
      check(std::abs(ImGui::GetIO().FontDefault->FontSize * ImGui::GetIO().FontGlobalScale - 13) < .01f,
        "Retina must preserve logical font size");
      std::vector<uint32_t> retina_buffer(2200 * 1440);
      check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
        retina_buffer.data(), 2200 * 4) == 0, "Retina readback");
      size_t retina_red = 0;
      for (auto c : retina_buffer)
        if (((c >> 16) & 255) > 220 && ((c >> 8) & 255) < 35 && (c & 255) < 35) ++retina_red;
      if (argc > 1) {
        auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(retina_buffer.data(), 2200, 1440,
          32, 2200 * 4, SDL_PIXELFORMAT_ARGB8888);
        auto path = std::filesystem::path(argv[1]); path.replace_extension("retina.bmp");
        check(SDL_SaveBMP(surface, path.string().c_str()) == 0, "save Retina visual QA"); SDL_FreeSurface(surface);
      }
      if (!(retina_red > red * 3.8 && retina_red < red * 4.2))
        std::cerr << "Red pixels: 1x=" << red << " 2x=" << retina_red << '\n';
      check(retina_red > red * 3.8 && retina_red < red * 4.2, "Retina video layout/scale");
      check(SDL_SetRenderTarget(renderer, nullptr) == 0, "restore render target");
      SDL_DestroyTexture(retina);
      ImGui_ImplSDLRenderer2_DestroyFontsTexture(); demo::apply_style(1);
      alice.tracks[0].muted = true; state.update(local, {alice, bob, guest});
      check(demo::tiles(state)[0].status == "Video muted", "mute placeholder"); render(state);
      alice.tracks.erase(alice.tracks.begin()); state.update(local, {alice, bob, guest});
      check(!state.frames.contains("TR_one") && state.frames.contains("TR_three"), "Stopping one stream removed another");
      state.changing_video = true; render(state); state.changing_video = false;
      state.update(local, {bob, guest}); check(!state.frames.contains("TR_one"), "departed pixels retained"); render(state);
      state.quality = quality_fixture;
      auto second_sender = state.quality.peers[0].streams.at("rtp");
      second_sender.id = "rtp-local2"; second_sender.ssrc = "4321"; second_sender.track_id = "video-2";
      second_sender.kbps = 160; second_sender.retransmit_kbps = 8; second_sender.fps = 15;
      state.quality.local_tracks["video-2"] = "TR_local2";
      state.quality.peers[0].streams.emplace(second_sender.id, second_sender);
      auto refresh_quality = [&] {
        state.quality.graph_sample.updated = std::chrono::steady_clock::now();
        for (auto& peer : state.quality.peers) peer.updated = std::chrono::steady_clock::now();
      };
      refresh_quality();
      state.quality.participants[bob.sid] = 2;
      state.received_frames = 1234;
      view.show_quality = true; render(state); render(state);
      auto save_dashboard = [&](const char* suffix, int w, int h) {
        if (argc <= 1) return;
        std::vector<uint32_t> pixels(w * h);
        check(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, pixels.data(), w * 4) == 0, "dashboard readback");
        auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
        auto path = std::filesystem::path(argv[1]); path.replace_extension(suffix);
        check(SDL_SaveBMP(surface, path.string().c_str()) == 0, "save dashboard QA"); SDL_FreeSurface(surface);
      };
      save_dashboard("quality.bmp", 1100, 720);
      quality_scroll = 430; render(state); render(state);
      save_dashboard("quality-media.bmp", 1100, 720);
      quality_scroll = 0; render(state); render(state);
      quality_scroll = -1;
      ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); render(state);
      check(!view.show_quality, "Escape closes quality dashboard");
      ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); render(state);
      view.show_quality = true;
      SDL_SetWindowSize(window, 640, 480); refresh_quality(); render(state);
      render(state); save_dashboard("quality-small.bmp", 640, 480);
      quality_scroll = 450; render(state); render(state);
      save_dashboard("quality-media-small.bmp", 640, 480);
      quality_scroll = -1;
      state.quality.peers[1].updated -= std::chrono::seconds(4); render(state);
      state.quality = {}; render(state); // waiting for statistics
      view.show_quality = false;
      SDL_SetWindowSize(window, 1100, 720);
      render({}); render({}); // idle view and texture teardown
      save_dashboard("join.bmp", 1100, 720);
    }
    ImGui_ImplSDLRenderer2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    std::cout << "Demo model, I420 and ImGui renderer tests passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
