#include <livekit/livekit.hpp>
#include "support/net.hpp"
#include <chrono>
#include <iostream>
#include <thread>

namespace sdk = livekit::sdk;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
static void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main(int argc, char** argv) {
  try {
    check(argc == 4, "Usage: region_integration server-url websocket-fixture-url http-fixture-url");
    const std::string server(argv[1]), fixture(argv[2]), discovery(argv[3]);
    const auto token = sdk::make_token("region-test", "region-integration-secret-32-characters", "region-client",
        {{"roomJoin", true}, {"room", "regions"}, {"canPublish", true}, {"canSubscribe", true}});
    sdk::RoomOptions options; options.regions.trusted_endpoints = {server};
    sdk::Room redirected;
    redirected.join_with_token(fixture + "/allowed", token, options);
    check(redirected.info().name == "regions" && redirected.local_participant().identity() == "region-client", "Trusted redirect failed");
    sdk::MediaSpec video; video.demo = true;
    auto publication = redirected.local_participant().publish_track(video).get();
    check(!publication.sid().empty(), "Publication after redirect failed");
    redirected.disconnect(); redirected.wait();
    // First-connect failures must not emit reconnect callbacks or retry the
    // same endpoint. Trust is separate from the ordered candidate list.
    for(const auto& path:{"/unavailable","/silent","/media-stall"}) {
      std::atomic<int> connected{0},reconnecting{0};sdk::RoomCallback callbacks;
      callbacks.on_connected=[&]{++connected;};callbacks.on_reconnecting=[&](const auto&){++reconnecting;};
      sdk::Room first(callbacks);sdk::RoomOptions initial=options;
      initial.connect_timeout=10;initial.regions.endpoint_timeout=250ms;initial.regions.fallback_endpoints={server,server};
      first.join_with_token(fixture+path,token,initial);
      const auto callback_deadline=Clock::now()+2s;
      while(connected==0 && Clock::now()<callback_deadline)std::this_thread::sleep_for(10ms);
      std::cerr<<"Initial endpoint "<<path<<" connected="<<connected.load()<<" reconnecting="<<reconnecting.load()<<" room="<<first.info().name<<'\n';
      check(connected==1 && reconnecting==0 && first.info().name=="regions","Initial endpoint failover failed");
      first.local_participant().publish_track(video).get();first.disconnect();first.wait();
    }
    for(const auto& path:{"/denied","/unauthorized","/malformed"}) {
      sdk::Room first;sdk::RoomOptions initial=options;initial.regions.fallback_endpoints={server};
      bool rejected=false;
      try{first.join_with_token(fixture+path,token,initial);}catch(const std::exception&){rejected=true;}
      check(rejected,"Initial failover bypassed a terminal error");
    }
    {
      sdk::Room first;sdk::RoomOptions initial=options;
      initial.regions.fallback_endpoints={server};initial.regions.initial_failover=false;
      bool rejected=false;try{first.join_with_token(fixture+"/unavailable",token,initial);}catch(const sdk::SDKError&){rejected=true;}
      check(rejected,"Disabled initial failover still switched endpoints");
      initial.regions.initial_failover=true;initial.regions.fallback_endpoints={fixture+"/forbidden"};
      rejected=false;try{first.join_with_token(fixture+"/unavailable",token,initial);}catch(const sdk::SDKError&){rejected=true;}
      check(rejected,"Untrusted fallback was accepted");
    }
    {
      sdk::Room first;sdk::RoomOptions initial=options;initial.connect_timeout=1;initial.regions.endpoint_timeout=300ms;
      initial.regions.trusted_endpoints.push_back(fixture+"/silent-2");initial.regions.fallback_endpoints={fixture+"/silent-2"};
      bool timed_out=false;const auto start=Clock::now();
      try{first.join_with_token(fixture+"/silent",token,initial);}catch(const sdk::SDKError& e){timed_out=e.code()==sdk::ErrorCode::timeout;}
      check(timed_out && Clock::now()-start<2500ms,"Initial failover total deadline exceeded");
    }
    {
      sdk::Room first;sdk::RoomOptions initial=options;initial.regions.fallback_endpoints={server};
      std::stop_source cancel;std::jthread stopper([&]{std::this_thread::sleep_for(100ms);cancel.request_stop();});
      const auto start=Clock::now();
      try{first.join_with_token(fixture+"/silent",token,initial,cancel.get_token());}catch(const std::exception&){}
      first.disconnect();first.wait();
      check(Clock::now()-start<2500ms && first.connection_state()==sdk::ConnectionState::disconnected,"Initial fallback cancellation failed");
    }
    auto rejected = [&](const std::string& path, sdk::ErrorCode expected, int status = 0) {
      sdk::Room room;
      try { room.join_with_token(fixture + path, token); }
      catch (const sdk::SDKError& e) {
        check(e.code() == expected && (!status || e.backend_reason() == status), "Wrong redirect failure"); return;
      }
      throw std::runtime_error("Redirect was not rejected");
    };
    rejected("/untrusted", sdk::ErrorCode::permission_denied);
    rejected("/cycle", sdk::ErrorCode::recovery_failed);
    rejected("/http", sdk::ErrorCode::connection_lost, 302);
    sdk::Room force_relay;
    sdk::RoomOptions no_turn; no_turn.disable_turn = true;
    bool relay_conflict = false;
    try { force_relay.join_with_token(fixture + "/force-relay", token, no_turn); }
    catch (const sdk::SDKError& e) { relay_conflict = e.code() == sdk::ErrorCode::unsupported; }
    check(relay_conflict, "Server relay policy was bypassed by disable_turn");
    const auto regions = lk::fetch_regions(discovery + "/settings-ok", token, {}, Clock::now() + 2s);
    check(regions == std::vector<std::string>{server}, "Region discovery parse failed");
    for (const auto& path : {"/settings-bad", "/settings-large", "/settings-redirect"}) {
      bool failed = false;
      try { lk::fetch_regions(discovery + path, token, {}, Clock::now() + 2s); }
      catch (const sdk::SDKError&) { failed = true; }
      check(failed, "Invalid region response accepted");
    }
    const auto start = Clock::now();
    bool timed_out = false;
    try { lk::fetch_regions(discovery + "/settings-slow", token, {}, start + 100ms); }
    catch (const sdk::SDKError& e) { timed_out = e.code() == sdk::ErrorCode::timeout; }
    check(timed_out && Clock::now() - start < 1500ms, "Region discovery deadline not bounded");
    std::stop_source stop;
    std::jthread cancel([&] { std::this_thread::sleep_for(100ms); stop.request_stop(); });
    bool cancelled = false;
    const auto cancel_start = Clock::now();
    try { lk::fetch_regions(discovery + "/settings-slow", token, stop.get_token(), cancel_start + 5s); }
    catch (const sdk::SDKError& e) { cancelled = e.code() == sdk::ErrorCode::cancelled; }
    check(cancelled && Clock::now() - cancel_start < 1500ms, "Region discovery cancellation not bounded");
    std::cout << sdk::Json{{"ok", true}, {"trustedRedirect", true}, {"httpRedirectRejected", true},
        {"discovery", true}, {"deadline", true}, {"cancelled", true}, {"relayConflictRejected", true},
        {"initialFailover",true},{"initialMediaFailover",true},{"initialFailoverTerminalErrors",true},{"initialFailoverDeadline",true},{"initialFailoverCancellation",true}}.dump() << '\n';
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
