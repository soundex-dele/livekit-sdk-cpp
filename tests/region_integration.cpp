#include "rtc_blocking_bridge.hpp"
#include "rtc_result.hpp"
using livekit::sdk::app_support::require_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
#include <livekit/rtc/livekit.hpp>
#include <livekit/auth.hpp>
#include "backends/rtc/livekit/signal_socket.hpp"
#include <chrono>
#include <iostream>
#include <thread>

namespace sdk = livekit::sdk;
namespace transport = sdk::rtc;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static void check(bool ok, const char* text) {
  if (!ok)
    throw std::runtime_error(text);
}

int main(int argc, char** argv) {
  try {
    check(argc == 4, "Usage: region_integration server-url websocket-fixture-url http-fixture-url");
    const std::string server(argv[1]), fixture(argv[2]), discovery(argv[3]);
    const auto token = sdk::make_token(
        "region-test", "region-integration-secret-32-characters", "region-client",
        {{"roomJoin", true}, {"room", "regions"}, {"canPublish", true}, {"canSubscribe", true}});
    transport::LiveKitRoomOptions options;
    options.token = token;
    options.regions.trusted_endpoints = {server};
    options.reconnect.enabled = false;
    auto join = [&](livekit::sdk::app_support::BlockingRoom& room, const std::string& url,
                    transport::LiveKitRoomOptions value) {
      value.url = url;
      return room.join(std::move(value));
    };
    transport::TrackDescriptor video;
    video.id = "video";
    video.format.codec = transport::Codec::vp8;
    video.format.width = 64;
    video.format.height = 48;
    {
      auto room = require_result(livekit::sdk::app_support::create_blocking_room());
      livekit::sdk::app_support::RtcEventMailbox room_events(*room);
      require_result(join(*room, fixture + "/allowed", options));
      const auto session = room->snapshot().session;
      check(
          session && session->room.name == "regions" && session->local.identity == "region-client",
          "Trusted redirect failed");
      auto publication = require_result(room->publish(video));
      check(!publication->snapshot().server_sid.empty(), "Publication after redirect failed");
      require_result(room->leave());
    }
    for (const auto& path : {"/unavailable", "/silent", "/media-stall"}) {
      auto room = require_result(livekit::sdk::app_support::create_blocking_room());
      livekit::sdk::app_support::RtcEventMailbox room_events(*room);
      auto initial = options;
      initial.connect_timeout = 10;
      initial.regions.endpoint_timeout = 250ms;
      initial.regions.fallback_endpoints = {server, server};
      require_result(join(*room, fixture + path, initial));
      int connected = 0, interrupted = 0;
      const auto notification_deadline = Clock::now() + 2s;
      while (Clock::now() < notification_deadline) {
        auto event = room_events.take_event(connected ? 0ms : 50ms);
        if (!event) {
          if (connected)
            break;
          else
            continue;
        }
        connected += event->kind == transport::RoomEventKind::connected;
        interrupted += event->kind == transport::RoomEventKind::reconnecting;
      }
      check(connected == 1 && interrupted == 0 && room->snapshot().session->room.name == "regions",
            "Initial endpoint failover failed");
      require_result(room->publish(video));
      require_result(room->leave());
    }
    for (const auto& path : {"/denied", "/unauthorized", "/malformed"}) {
      auto room = require_result(livekit::sdk::app_support::create_blocking_room());
      auto initial = options;
      initial.regions.fallback_endpoints = {server};
      bool rejected = false;
      try {
        require_result(join(*room, fixture + path, initial));
      } catch (const std::exception&) {
        rejected = true;
      }
      check(rejected, "Initial failover bypassed a terminal error");
    }
    {
      auto room = require_result(livekit::sdk::app_support::create_blocking_room());
      auto initial = options;
      initial.connect_timeout = 1;
      initial.regions.endpoint_timeout = 5s;
      initial.regions.trusted_endpoints.push_back(fixture + "/silent-2");
      initial.regions.fallback_endpoints = {fixture + "/silent-2"};
      bool timed_out = false;
      const auto start = Clock::now();
      try {
        require_result(join(*room, fixture + "/silent", initial));
      } catch (const sdk::SDKError& error) {
        timed_out = error.code() == sdk::ErrorCode::timeout;
      }
      check(timed_out && Clock::now() - start < 2500ms, "Initial failover total deadline exceeded");
    }
    {
      auto room = require_result(livekit::sdk::app_support::create_blocking_room());
      auto initial = options;
      initial.regions.fallback_endpoints = {server};
      const auto start = Clock::now();
      initial.connect_timeout = 1;
      initial.regions.fallback_endpoints.clear();
      auto joining =
          std::async(std::launch::async, [&] { return join(*room, fixture + "/silent", initial); });
      std::this_thread::sleep_for(100ms);
      auto leaving = std::async(std::launch::async, [&] { return room->leave(); });
      check(leaving.wait_for(2s) == std::future_status::ready,
            "Leave did not interrupt initial connection");
      auto result = joining.get();
      require_result(leaving.get());
      check(result == sdk::ErrorCode::cancelled && Clock::now() - start < 2500ms &&
                room->snapshot().state == transport::ConnectionState::closed,
            "Asynchronous cancellation/leave failed");
    }
    auto rejected = [&](const std::string& path, sdk::ErrorCode expected, int status = 0) {
      auto room = require_result(livekit::sdk::app_support::create_blocking_room());
      auto untrusted = options;
      untrusted.regions.trusted_endpoints.clear();
      try {
        require_result(join(*room, fixture + path, untrusted));
      } catch (const sdk::SDKError& error) {
        check(error.code() == expected && (!status || error.backend_reason() == status),
              "Wrong redirect failure");
        return;
      }
      throw std::runtime_error("Redirect was not rejected");
    };
    rejected("/untrusted", sdk::ErrorCode::permission_denied);
    rejected("/cycle", sdk::ErrorCode::recovery_failed);
    rejected("/http", sdk::ErrorCode::connection_lost, 302);
    auto force_relay = require_result(livekit::sdk::app_support::create_blocking_room());
    auto direct = options;
    direct.media_transport = sdk::MediaTransport::udp_only;
    bool relay_conflict = false;
    try {
      require_result(join(*force_relay, fixture + "/force-relay", direct));
    } catch (const sdk::SDKError& error) {
      relay_conflict = error.code() == sdk::ErrorCode::unsupported;
    }
    check(relay_conflict, "Server relay policy was bypassed by strict direct transport");
    const auto regions =
        lk::fetch_regions(discovery + "/settings-ok", token, {}, Clock::now() + 2s);
    check(regions == std::vector<std::string>{server}, "Region discovery parse failed");
    for (const auto& path : {"/settings-bad", "/settings-large", "/settings-redirect"}) {
      bool failed = false;
      try {
        lk::fetch_regions(discovery + path, token, {}, Clock::now() + 2s);
      } catch (const sdk::SDKError&) {
        failed = true;
      }
      check(failed, "Invalid region response accepted");
    }
    const auto start = Clock::now();
    bool timed_out = false;
    try {
      lk::fetch_regions(discovery + "/settings-slow", token, {}, start + 100ms);
    } catch (const sdk::SDKError& e) {
      timed_out = e.code() == sdk::ErrorCode::timeout;
    }
    check(timed_out && Clock::now() - start < 1500ms, "Region discovery deadline not bounded");
    std::stop_source stop;
    std::jthread cancel([&] {
      std::this_thread::sleep_for(100ms);
      stop.request_stop();
    });
    bool cancelled = false;
    const auto cancel_start = Clock::now();
    try {
      lk::fetch_regions(discovery + "/settings-slow", token, stop.get_token(), cancel_start + 5s);
    } catch (const sdk::SDKError& e) {
      cancelled = e.code() == sdk::ErrorCode::cancelled;
    }
    check(cancelled && Clock::now() - cancel_start < 1500ms,
          "Region discovery cancellation not bounded");
    std::cout << sdk::Json{{"ok", true},
                           {"trustedRedirect", true},
                           {"httpRedirectRejected", true},
                           {"discovery", true},
                           {"deadline", true},
                           {"cancelled", true},
                           {"relayConflictRejected", true},
                           {"initialFailover", true},
                           {"initialMediaFailover", true},
                           {"initialFailoverTerminalErrors", true},
                           {"initialFailoverDeadline", true},
                           {"initialFailoverCancellation", true}}
                     .dump()
              << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
