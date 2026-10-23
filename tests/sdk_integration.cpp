#include <livekit/livekit.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace sdk = livekit::sdk;
static std::string env(const char* key) { auto value = std::getenv(key); return value ? value : ""; }
static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void eventually(F predicate, const char* message) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!predicate()) { if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error(message); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
}
template<class T> T result(std::future<T> future) {
  check(future.wait_for(std::chrono::seconds(20)) == std::future_status::ready, "Command did not complete"); return future.get();
}
int main(int argc, char** argv) {
  try {
    check(argc == 2, "Usage: livekit_sdk_integration RECORD_DIRECTORY");
    const auto url = env("LIVEKIT_URL"), key = env("LIVEKIT_API_KEY"), secret = env("LIVEKIT_API_SECRET");
    const std::string name = "sdk-in-process";
    sdk::RoomServiceClient service(url, {key, secret, {}});
    service.create_room(name);
    struct DeleteRoom { sdk::RoomServiceClient& service; std::string name; ~DeleteRoom() { try { service.delete_room(name); } catch (...) {} } } cleanup{service, name};
    auto options = [&](const char* identity) {
      sdk::RoomOptions o; o.url = url; o.room = name; o.identity = identity;
      o.token = sdk::make_token(key, secret, identity, {{"roomJoin", true}, {"room", name}, {"canPublish", true}, {"canSubscribe", true}, {"canPublishData", true}});
      return o;
    };
    sdk::Room subscriber, publisher;
    auto sub = options("sdk-subscriber"); sub.record = std::filesystem::u8path(argv[1]);
    std::promise<void> ready; auto connected = ready.get_future();
    std::exception_ptr receive_failure;
    bool ready_set = false;
    int tracks = 0, messages = 0;
    std::jthread receiving([&](std::stop_token stop) {
      try {
        auto thread = std::this_thread::get_id();
        subscriber.run(sub, [&](const sdk::Json& e) {
          check(thread == std::this_thread::get_id(), "Wrong subscriber callback thread");
          if (e.at("event") == "connected") { ready.set_value(); ready_set = true; }
          if (e.at("event") == "track-subscribed") ++tracks;
          if (e.at("event") == "data" && e.at("payloadBase64") == sdk::base64url("SDK message")) ++messages;
        }, stop);
      } catch (...) {
        receive_failure = std::current_exception();
        if (!ready_set) ready.set_exception(receive_failure);
      }
    });
    check(connected.wait_for(std::chrono::seconds(30)) == std::future_status::ready, "Subscriber connect timeout");
    connected.get();
    auto pub = options("sdk-publisher"); pub.duration = 4; pub.publish_data = "SDK message";
    pub.media.push_back({"", "ffmpeg", true, true});
    pub.media.push_back({"", "ffmpeg", false, true});
    bool reentry_rejected = false, publisher_connected = false;
    publisher.run(pub, [&](const sdk::Json& e) {
      if (e.at("event") == "connected") {
        publisher_connected = true;
        try { publisher.run(pub); } catch (const std::logic_error&) { reentry_rejected = true; }
      }
    });
    subscriber.request_stop(); receiving.join();
    if (receive_failure) std::rethrow_exception(receive_failure);
    check(publisher_connected && reentry_rejected && tracks == 2 && messages == 1, "SDK concurrent media/data/reentry failure");

    // A callback may stop its own room. Reusing the same object must reset cancellation.
    auto reuse = options("sdk-reuse");
    int runs = 0;
    for (int i = 0; i < 2; ++i) subscriber.run(reuse, [&](const sdk::Json& e) {
      if (e.at("event") == "connected") { ++runs; subscriber.request_stop(); }
    });
    check(runs == 2, "Room reuse after cancellation");
    bool callback_failed = false;
    try { subscriber.run(reuse, [](const sdk::Json& e) {
      if (e.at("event") == "connected") throw std::runtime_error("expected callback exception");
    }); } catch (const std::runtime_error& e) { callback_failed = std::string(e.what()) == "expected callback exception"; }
    check(callback_failed, "Callback exception propagation");
    // Go-style nonblocking client API, typed events, and dynamic publications.
    std::atomic<int> published{0}, subscribed{0}, unpublished{0}, received{0}, metadata_events{0}, attributes_events{0};
    std::atomic<bool> binary_ok{false}, lossy_ok{false}, server_data{false};
    sdk::RoomCallback cb;
    cb.on_track_published = [&](const auto&, const auto&) { ++published; };
    cb.on_track_subscribed = [&](const auto&, const auto&) { ++subscribed; };
    cb.on_track_unpublished = [&](const auto&, const auto&) { ++unpublished; };
    cb.on_metadata_changed = [&](const auto&, const auto&) { ++metadata_events; };
    cb.on_attributes_changed = [&](const auto&, const auto&) { ++attributes_events; };
    cb.on_data_received = [&](const auto& bytes, const sdk::DataReceiveParams& params) {
      ++received;
      if (params.topic == "binary" && params.reliable && bytes == std::vector<uint8_t>{0, 255, 1, 0}) binary_ok = true;
      if (params.topic == "lossy" && !params.reliable) lossy_ok = true;
      if (params.topic == "server") server_data = true;
    };
    sdk::Room receiver(cb), sender;
    auto token_for = [&](const char* id) { return sdk::AccessToken(key, secret).set_identity(id).set_name("initial")
      .set_metadata("original").set_attributes({{"role", "tester"}}).set_video_grant({{"roomJoin", true}, {"room", name}, {"canUpdateOwnMetadata", true}}).to_jwt(); };
    receiver.join_with_token(url, token_for("async-receiver")); // room inferred from token
    sender.join_with_token(url, token_for("async-sender"));
    check(sender.connection_state() == sdk::ConnectionState::connected && sender.info().name == name, "Join/state");
    check(sender.participant_by_identity("async-receiver").has_value(), "Initial participant roster");
    eventually([&] { return receiver.participant_by_sid(sender.local_participant().sid()).has_value(); }, "Participant lookup");
    auto local = sender.local_participant();
    auto video = result(local.publish_track({"", "ffmpeg", true, true}, {"dynamic-video", "test-stream", sdk::TrackSource::screen_share}));
    auto audio = result(local.publish_track({"", "ffmpeg", false, true}, {"dynamic-audio"}));
    check(video.info().name == "dynamic-video" && video.info().source == sdk::TrackSource::screen_share, "Publication options");
    eventually([&] { return published >= 2 && subscribed >= 2; }, "Dynamic publish/subscription events");
    auto remote = receiver.participant_by_identity("async-sender").value();
    auto pubs = remote.track_publications(); check(pubs.size() == 2, "Remote publication model");
    auto remote_video = *std::find_if(pubs.begin(), pubs.end(), [&](const auto& p) { return p.sid() == video.sid(); });
    result(remote_video.set_subscribed(false));
    eventually([&] { return !remote_video.info().subscribed; }, "Unsubscribe state");
    result(remote_video.set_subscribed(true));
    eventually([&] { return remote_video.info().subscribed; }, "Resubscribe state");
    result(remote_video.set_enabled(false)); result(remote_video.set_video_dimensions(320, 180)); result(remote_video.set_enabled(true));
    result(video.set_muted(true)); eventually([&] { return remote_video.info().muted; }, "Remote mute state");
    result(video.set_muted(false)); eventually([&] { return !remote_video.info().muted; }, "Remote unmute state");
    result(local.publish_data({0, 255, 1, 0}, {true, "binary", {"async-receiver"}}));
    eventually([&] { return binary_ok.load(); }, "Reliable targeted binary data");
    // Lossy delivery can be dropped; retry a bounded number to exercise the channel.
    for (int i = 0; i < 10 && !lossy_ok; ++i) { result(local.publish_data({42}, {false, "lossy"})); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    check(lossy_ok, "Lossy data channel");
    result(local.set_metadata("updated")); eventually([&] { return remote.info().metadata == "updated"; }, "Metadata propagation");
    result(local.set_name("renamed")); eventually([&] { return remote.info().name == "renamed"; }, "Name propagation");
    result(local.set_attributes({{"role", ""}, {"color", "blue"}}));
    eventually([&] { auto a = remote.info().attributes; return !a.contains("role") && a.contains("color"); }, "Attribute delta/deletion");
    check(metadata_events > 0 && attributes_events > 0, "Typed metadata/attribute callbacks");
    sdk::LiveKitAPI api({url, {key, secret, {}}, false});
    api.room().send_data(sdk::Json{{"room", name}, {"data", "aGk="}, {"kind", "RELIABLE"}, {"topic", "server"}, {"destinationIdentities", {"async-receiver"}}});
    eventually([&] { return server_data.load(); }, "Server API data");
    api.room().update_room_metadata(name, "room-updated");
    eventually([&] { return receiver.info().metadata == "room-updated"; }, "Room metadata callback/state");
    result(local.unpublish_track(video.sid())); result(local.unpublish_track(audio.sid()));
    eventually([&] { return unpublished >= 2 && remote.track_publications().empty(); }, "Dynamic unpublish");
    auto again = result(local.publish_track({"", "ffmpeg", false, true}, {"republished"}));
    eventually([&] { return remote.track_publications().size() == 1; }, "Republish after renegotiation");
    result(local.unpublish_track(again.sid()));
    auto pending = local.publish_track({"", "ffmpeg", true, true});
    sender.disconnect();
    check(pending.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "Pending publication cancellation");
    try { pending.get(); } catch (const std::exception&) {}
    bool stale = false; try { result(local.publish_data({1})); } catch (const std::logic_error&) { stale = true; }
    check(stale, "Disconnected handles must reject commands");
    sender.join_with_token(url, token_for("async-sender"));
    stale = false; try { result(local.publish_data({1})); } catch (const std::logic_error&) { stale = true; }
    check(stale, "Previous generation handle must not act on new session");
    api.room().remove_participant(name, "async-sender");
    sender.wait(); check(sender.disconnect_reason() == 4, "Server disconnect reason");
    receiver.disconnect(); receiver.wait();
    // A subscribe-only token must not require a publisher transport/permission.
    auto viewer_token = sdk::AccessToken(key, secret).set_identity("viewer-only").set_video_grant({{"roomJoin", true}, {"room", name}, {"canPublish", false}, {"canPublishData", false}, {"canSubscribe", true}}).to_jwt();
    receiver.join_with_token(url, viewer_token); receiver.disconnect(); receiver.wait();
    std::cout << sdk::Json{{"ok", true}, {"tracks", tracks}, {"dataMessages", messages}, {"reusedSessions", runs}, {"dynamicPublished", published.load()}, {"dynamicSubscribed", subscribed.load()}, {"dynamicUnpublished", unpublished.load()}, {"typedDataMessages", received.load()}, {"goCore", true}}.dump() << '\n';
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
