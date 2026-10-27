#include <livekit/livekit.hpp>
#include <cstdlib>
#include <iostream>
#include <map>

int main() {
  namespace sdk = livekit::sdk;
  using namespace std::chrono_literals;
  try {
    const auto url = std::getenv("LIVEKIT_URL"), token = std::getenv("LIVEKIT_TOKEN");
    if (!url || !token) throw std::runtime_error("Set LIVEKIT_URL and a room-join LIVEKIT_TOKEN");
    auto stream = std::make_shared<sdk::AudioFrameStream>();
    sdk::RoomOptions options; options.audio_frame_stream = stream;
    sdk::Room room; room.join_with_token(url, token, options);
    std::map<std::string, uint64_t> samples;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    size_t blocks = 0;
    while (blocks < 100 && std::chrono::steady_clock::now() < deadline) {
      auto next = stream->read(1s);
      if (next.status == sdk::AudioReadStatus::closed) break;
      if (next.status != sdk::AudioReadStatus::frame) continue;
      // Application processing happens here, outside SDK/native callbacks.
      // Keep track generations and sample_index when maintaining continuity.
      samples[next.frame->track_id] += next.frame->samples_per_channel; ++blocks;
    }
    room.disconnect(); room.wait();
    for (const auto& [track, count] : samples) std::cout << track << " samples/channel=" << count << '\n';
    std::cout << "dropped_blocks=" << stream->stats().dropped << '\n';
    return blocks ? 0 : 1;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
