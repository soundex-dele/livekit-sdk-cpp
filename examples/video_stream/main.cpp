#include <livekit/livekit.hpp>
#include <cstdlib>
#include <iostream>

int main() {
  namespace sdk = livekit::sdk;
  using namespace std::chrono_literals;
  try {
    const auto url = std::getenv("LIVEKIT_URL"), token = std::getenv("LIVEKIT_TOKEN");
    if (!url || !token) throw std::runtime_error("Set LIVEKIT_URL and a room-join LIVEKIT_TOKEN");
    sdk::RoomOptions options;
    auto stream = std::make_shared<sdk::VideoFrameStream>(sdk::VideoStreamOptions{3, 16 * 1024 * 1024});
    options.video_frame_stream = stream;
    sdk::Room room;
    room.join_with_token(url, token, options);
    size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (received < 50 && std::chrono::steady_clock::now() < deadline) {
      auto next = stream->read(1s);
      if (next.status == sdk::VideoReadStatus::closed) break;
      if (next.status != sdk::VideoReadStatus::frame) continue;
      // Process next.frame on this application thread. Its immutable I420
      // buffers may be retained; generation changes after a full rejoin.
      if (next.frame->y.empty()) throw std::runtime_error("Empty decoded video");
      ++received;
    }
    room.disconnect(); room.wait();
    std::cout << "frames=" << received << " dropped=" << stream->stats().dropped << '\n';
    return received ? 0 : 1;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
