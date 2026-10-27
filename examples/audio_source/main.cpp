#include <livekit/livekit.hpp>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>

int main(int argc, char** argv) {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  if (argc != 3) { std::cerr << "Usage: audio_source <server-url> <token>\n"; return 2; }
  try {
    auto source = std::make_shared<PcmAudioSource>(PcmSourceOptions{44100,1,AudioSampleFormat::float32});
    Room room; room.join_with_token(argv[1], argv[2]);
    MediaSpec media; media.video = false; media.audio_source = source;
    auto track = room.local_participant().publish_track(media, {"application-audio"}).get();
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    for (int block = 0; block < 500;) {
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Audio source did not make progress before deadline");
      PcmInputFrame frame; frame.float_samples.resize(882);
      for (size_t i = 0; i < 882; ++i)
        frame.float_samples[i] = static_cast<float>(.2 * std::sin(2 * std::numbers::pi * 440 * (block * 882 + i) / 44100));
      auto status = source->write(frame);
      if (status == AudioWriteStatus::accepted) ++block;
      else if (status == AudioWriteStatus::closed) break;
      std::this_thread::sleep_for(2ms);
    }
    // Let the admitted tail reach the native encoder; this is not a remote ACK.
    while (source->stats().queued_blocks) {
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Audio source did not drain before deadline");
      std::this_thread::sleep_for(10ms);
    }
    source->close(); room.local_participant().unpublish_track(track.sid()).get();
    room.disconnect(); room.wait();
    std::cout << "Accepted samples per channel: " << source->stats().accepted_samples << '\n';
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
