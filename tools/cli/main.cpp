#include "livekit/api.hpp"
#include "session.hpp"
#include <thread>
#include <chrono>
#include <atomic>
#include <CLI/CLI.hpp>
#include <cstdlib>
#include <iostream>
#include <csignal>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
namespace lk = livekit::sdk;
namespace {
std::string env(const char* key) { auto value = std::getenv(key); return value ? value : ""; }
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> interrupted{false};
void signal_handler(int) { interrupted.store(true, std::memory_order_relaxed); }
void print_event(const lk::Json& event) { std::cout << event.dump() << '\n' << std::flush; }
}
int run(int argc, char** argv) {
  CLI::App app{"Native C++ LiveKit CLI: rooms, publish, subscribe"};
  app.set_version_flag("--version", "lk-cpp 0.3.0 (livekit-sdk-cpp)"); app.require_subcommand();
  std::string url, key, secret, token;
  auto resolve_credentials = [&] {
    if (url.empty()) url = env("LIVEKIT_URL");
    if (key.empty() && secret.empty() && token.empty()) {
      token = env("LIVEKIT_TOKEN");
      if (token.empty()) { key = env("LIVEKIT_API_KEY"); secret = env("LIVEKIT_API_SECRET"); }
    }
  };
  app.add_option("--url", url, "LiveKit http(s)/ws(s) URL; LIVEKIT_URL");
  app.add_option("--api-key", key, "API key; LIVEKIT_API_KEY");
  app.add_option("--api-secret", secret, "API secret; LIVEKIT_API_SECRET");
  app.add_option("--token", token, "Pre-issued bearer JWT; LIVEKIT_TOKEN");
  bool json_output = false; app.add_flag("--json", json_output, "JSON output (already the default)");
  auto service = [&] { resolve_credentials(); return lk::LiveKitAPI({url, {key, secret, token}, false}).room(); };
  auto output = [](const lk::Json& value) { std::cout << value.dump(2) << '\n'; };
  auto token_cmd = app.add_subcommand("token", "Access tokens")->require_subcommand()->fallthrough();
  auto create_token = token_cmd->add_subcommand("create", "Create an HS256 room join token")->fallthrough();
  std::string token_room, token_identity, token_name; int ttl = 3600; bool no_publish = false, no_subscribe = false, join_flag = false;
  create_token->add_option("--room", token_room)->required(); create_token->add_option("--identity", token_identity)->required();
  create_token->add_option("--name", token_name); create_token->add_option("--ttl", ttl, "Lifetime in seconds")->check(CLI::PositiveNumber);
  create_token->add_flag("--join", join_flag, "Grant room join (default)");
  create_token->add_flag("--no-publish", no_publish); create_token->add_flag("--no-subscribe", no_subscribe);
  create_token->callback([&] { resolve_credentials(); std::cout << lk::make_token(key, secret, token_identity, {{"roomJoin", true}, {"room", token_room}, {"canPublish", !no_publish}, {"canSubscribe", !no_subscribe}, {"canPublishData", !no_publish}}, ttl, token_name) << '\n'; });
  auto room = app.add_subcommand("room", "Manage rooms and publish/subscribe to media")->require_subcommand()->fallthrough();
  std::vector<std::string> names;
  auto list = room->add_subcommand("list", "List active rooms")->fallthrough(); list->add_option("names", names);
  list->callback([&] { output(service().list_rooms(names)); });
  std::string room_name, metadata, identity, track_sid;
  unsigned empty_timeout = 300, max_participants = 0;
  auto create = room->add_subcommand("create", "Create a room")->fallthrough(); create->add_option("name", room_name)->required();
  create->add_option("--empty-timeout", empty_timeout); create->add_option("--max-participants", max_participants); create->add_option("--metadata", metadata);
  create->callback([&] { output(service().create_room(room_name, empty_timeout, max_participants, metadata)); });
  auto remove = room->add_subcommand("delete", "Delete a room and disconnect participants")->fallthrough(); remove->add_option("name", room_name)->required();
  remove->callback([&] { output(service().delete_room(room_name)); });
  auto update = room->add_subcommand("update", "Update room metadata")->fallthrough(); update->add_option("name", room_name)->required(); update->add_option("--metadata", metadata)->required();
  update->callback([&] { output(service().update_room_metadata(room_name, metadata)); });
  auto participants = room->add_subcommand("participants", "List participants")->fallthrough(); participants->add_option("room", room_name)->required();
  participants->callback([&] { output(service().list_participants(room_name)); });
  auto participant = room->add_subcommand("participant", "Get one participant")->fallthrough(); participant->add_option("room", room_name)->required(); participant->add_option("--identity", identity)->required();
  participant->callback([&] { output(service().get_participant(room_name, identity)); });
  auto kick = room->add_subcommand("remove-participant", "Disconnect a participant")->fallthrough(); kick->add_option("room", room_name)->required(); kick->add_option("--identity", identity)->required();
  kick->callback([&] { output(service().remove_participant(room_name, identity)); });
  auto mute = room->add_subcommand("mute-track", "Mute a published track")->fallthrough(); mute->add_option("room", room_name)->required(); mute->add_option("--identity", identity)->required(); mute->add_option("--sid", track_sid)->required();
  bool unmute = false; mute->add_flag("--unmute", unmute);
  mute->callback([&] { output(service().mute_published_track(room_name, identity, track_sid, !unmute)); });
  livekit::cli::SessionOptions options; std::vector<std::string> publish; std::string ffmpeg = "ffmpeg", record;
  bool demo = false, tone = false, loop = false, no_auto_subscribe = false;
  int width = 640, height = 360, fps = 30;
  for (const auto& command : {"join", "publish", "subscribe"}) {
    auto join = room->add_subcommand(command, "Join room; publish files/streams or subscribe/record")->fallthrough();
    join->add_option("room", options.room)->required(); join->add_option("--identity", options.identity);
    join->add_option("--publish", publish, "File, RTSP/HTTP URL, h264/h265/vp8/opus://host:port; repeatable")->expected(1)->take_all();
    join->add_flag("--publish-demo", demo, "Publish generated video (no FFmpeg needed)");
    join->add_flag("--publish-tone", tone, "Publish 440 Hz test audio (no FFmpeg needed)");
    join->add_flag("--microphone", options.microphone, "Publish default system microphone");
    join->add_flag("--audio-playout", options.audio_playout, "Play subscribed audio on default output device");
    join->add_flag("--video-playout", options.video_playout, "Display subscribed video in SDL windows; close or Esc to leave");
    join->add_flag("--no-subscribe", no_auto_subscribe, "Disable automatic subscriptions");
    join->add_option("--subscribe-track", options.subscribe_tracks, "Subscribe only to specified track SIDs (repeatable)")->expected(1)->take_all();
    join->add_option("--record", record, "Output directory: per-track WAV and Y4M files");
    join->add_option("--ffmpeg", ffmpeg, "FFmpeg path; ffprobe must be beside it");
    join->add_flag("--loop", loop, "Loop finite file inputs");
    join->add_flag("--exit-after-publish", options.exit_after_publish, "Exit when all media inputs finish");
    join->add_option("--width", width)->check(CLI::Range(2, 3840)); join->add_option("--height", height)->check(CLI::Range(2, 2160));
    join->add_option("--fps", fps)->check(CLI::Range(1, 60));
    join->add_option("--codec", options.codec, "Video codec")->check(CLI::IsMember({"vp8", "h264", "h265"}));
    join->add_option("--duration", options.duration, "Exit after N connected seconds (0: until Ctrl+C)")->check(CLI::NonNegativeNumber);
    join->add_option("--connect-timeout", options.connect_timeout)->check(CLI::PositiveNumber);
    join->add_option("--stats-interval-ms", options.stats_interval_ms, "Native RTC media/transport stats (0 disables; otherwise 200..60000 ms)")->check(CLI::Range(0, 60000));
    join->add_option("--publish-data", options.publish_data, "Send a reliable UTF-8 data message"); join->add_option("--topic", options.topic);
    join->callback([&, command = std::string(command)] {
      resolve_credentials();
      if (width % 2 || height % 2) throw std::runtime_error("Width and height must be even");
      if (options.identity.empty() && token.empty()) throw std::runtime_error("--identity is required when generating a room token");
      if (!options.subscribe_tracks.empty() && no_auto_subscribe) throw std::runtime_error("--subscribe-track conflicts with --no-subscribe");
      if (options.video_playout && no_auto_subscribe) throw std::runtime_error("--video-playout conflicts with --no-subscribe");
      if (options.exit_after_publish && (demo || tone || loop || options.microphone || publish.empty())) throw std::runtime_error("--exit-after-publish requires finite --publish inputs without --loop/demo/tone/microphone");
      if (command == "publish" && publish.empty() && !demo && !tone && !options.microphone && options.publish_data.empty()) throw std::runtime_error("room publish requires a media or data source");
      if (command == "subscribe" && (!publish.empty() || demo || tone || options.microphone || !options.publish_data.empty())) throw std::runtime_error("Use room join/publish to publish media");
      options.url = lk::endpoint(url, true); options.subscribe = !no_auto_subscribe; options.record = std::filesystem::u8path(record);
      if (!record.empty() && !options.subscribe) throw std::runtime_error("--record requires subscriptions");
      options.token = token.empty() ? lk::make_token(key, secret, options.identity, {{"roomJoin", true}, {"room", options.room}, {"canPublish", true}, {"canSubscribe", true}, {"canPublishData", true}}) : token;
      for (const auto& input : publish) { auto streams = lk::probe_media(input, ffmpeg, loop, width, height, fps); options.media.insert(options.media.end(), streams.begin(), streams.end()); }
      if (demo) options.media.push_back({"", ffmpeg, true, true, false, width, height, fps});
      if (tone) options.media.push_back({"", ffmpeg, false, true, false, width, height, fps});
      std::signal(SIGINT, signal_handler); std::signal(SIGTERM, signal_handler);
      std::stop_source session_stop;
      std::jthread cancellation([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
          if (interrupted) { session_stop.request_stop(); return; }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      });
      livekit::cli::run_session(options,print_event,session_stop.get_token());
    });
  }
  auto doctor = app.add_subcommand("doctor", "Create a transport-only native WebRTC peer and generate SDP"); doctor->callback([] { livekit::cli::doctor(print_event); });
  try { app.parse(argc, argv); }
  catch (const CLI::ParseError& e) { return app.exit(e); }
  catch (const std::exception& e) { std::cerr << "error: " << e.what() << '\n'; return 1; }
  return interrupted ? 130 : 0;
}
int main(int argc, char** argv) {
#ifdef _WIN32
  // Windows argv is otherwise decoded in the active ANSI code page.
  int count = 0; auto wide = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!wide) return 1;
  std::vector<std::string> utf8; utf8.reserve(count);
  for (int i = 0; i < count; ++i) {
    auto size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    std::string value(size, '\0'); WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), size, nullptr, nullptr); value.pop_back(); utf8.push_back(std::move(value));
  }
  LocalFree(wide);
  std::vector<char*> args; for (auto& value : utf8) args.push_back(value.data()); args.push_back(nullptr);
  return run(count, args.data());
#else
  return run(argc, argv);
#endif
}
