#include <livekit/livekit.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  if (argc != 3) { std::cerr << "Usage: video_source <server-url> <token>\n"; return 2; }
  try {
    auto source = std::make_shared<I420VideoSource>();
    Room room; room.join_with_token(argv[1], argv[2]);
    MediaSpec media; media.video_source = source;
    auto track = room.local_participant().publish_track(media, {"application-video"}).get();
    VideoFrame frame; frame.width = 640; frame.height = 360;
    frame.stride_y = 640; frame.stride_u = frame.stride_v = 320;
    frame.y.resize(640 * 360); frame.u.assign(320 * 180, 96); frame.v.assign(320 * 180, 160);
    for (int i = 0; i < 300; ++i) {
      std::fill(frame.y.begin(), frame.y.end(), static_cast<uint8_t>(16 + i % 200));
      if (source->write(frame) == VideoWriteStatus::closed) break;
      std::this_thread::sleep_for(34ms); // Backpressure: skip this live frame.
    }
    source->close(); room.local_participant().unpublish_track(track.sid()).get();
    room.disconnect(); room.wait();
    std::cout << "Accepted frames: " << source->stats().accepted << '\n';
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
