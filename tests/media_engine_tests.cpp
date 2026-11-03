#include "livekit/media_engine.hpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <type_traits>

using namespace livekit::sdk;
using namespace livekit::sdk::client;
using namespace std::chrono_literals;

static_assert(
    std::is_same_v<decltype(std::declval<MediaEngine>().connect(ConnectionOptions{})), bool>);
static_assert(std::is_same_v<decltype(std::declval<MediaEngine>().close()), void>);
static_assert(
    std::is_same_v<decltype(std::declval<MediaEngine>().publish_media(MediaPublishSpec{})),
                   Result<MediaPublication>>);
static_assert(std::is_same_v<decltype(std::declval<MediaEngine>().set_muted("", true)), void>);

void check(bool value, const char* text) {
  if (!value) {
    std::cerr << text << '\n';
    std::abort();
  }
}

struct Observer : RoomObserver {
  MediaEngine* engine;
  std::atomic<bool> connecting{false}, rejected{false};

  void on_room_event(const RoomEvent& event) override {
    if (event.connection == ConnectionState::connecting) {
      connecting = true;
      auto result = engine->publish_media({});
      rejected = !result && result.error().code == ErrorCode::invalid_thread;
      (void)engine->info();
    }
  }
};

int main() {
  Observer observer;
  MediaEngine engine;
  observer.engine = &engine;
  engine.set_observer(&observer);
  check(engine.connection_state() == ConnectionState::created, "initial state");
  check(!engine.publish_media({}), "publish before connect rejected");
  check(!engine.subscribe_media({}), "subscribe before connect rejected");
  ConnectionOptions options;
  options.url = "ws://127.0.0.1:1";
  options.token = "test";
  options.connect_timeout = 2;
  std::atomic<bool> connected{true};
  std::thread connector([&] { connected = engine.connect(options); });
  auto deadline = SteadyClock::now() + 3s;
  while (!observer.connecting && SteadyClock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  engine.close();
  connector.join();
  engine.set_observer(nullptr);
  check(!connected, "cancelled/failed connection returns false");
  check(observer.rejected, "observer synchronous reentry rejected");
  check(engine.connection_state() == ConnectionState::closed, "close fences Room");
  check(!engine.connect(options), "closed engine cannot reconnect");
  engine.close();
  std::cout << "PASS media engine synchronous contract and lifecycle\n";
}
