#include <livekit/rtc/engine.hpp>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>

using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
  if (argc != 3 && (argc != 4 || std::string(argv[3]) != "--microphone")) {
    std::cerr << "Usage: audio_source <server-url> <token> [--microphone]\n";
    return 2;
  }
  try {
    auto engine = engine::create_livekit_engine({});
    RoomOptions options;
    options.url = argv[1];
    options.token = argv[2];
    engine->join(options).get();
    TrackDescriptor descriptor;
    descriptor.id = descriptor.name = "application-audio";
    descriptor.kind = MediaKind::audio;
    descriptor.format.codec = Codec::opus;
    descriptor.format.clock_rate = 48000;
    descriptor.format.channels = 1;
    engine::MediaInput input;
    if (argc == 4) {
      input.microphone = livekit::sdk::MicrophoneOptions{};
      descriptor.source = PublicationSource::microphone;
    } else {
      livekit::sdk::PcmSourceOptions pcm;
      pcm.sample_rate = 44100;
      pcm.format = livekit::sdk::AudioSampleFormat::float32;
      input.audio_source = std::make_shared<livekit::sdk::PcmAudioSource>(pcm);
    }
    engine::MediaPublishSpec config;
    config.track = descriptor;
    config.input = input;
    auto operation = engine->publish_audio(std::move(config));
    auto publication = std::move(operation.ready);
    while (publication.wait_for(0ms) != std::future_status::ready) {
      engine->poll();
      std::this_thread::sleep_for(1ms);
    }
    const auto id = publication.get();
    const auto start = SteadyClock::now();
    for (uint64_t block = 0; block < 1000; ++block) {
      if (input.audio_source) {
        livekit::sdk::PcmInputFrame pcm;
        pcm.float_samples.resize(441);
        for (size_t i = 0; i < 441; ++i)
          pcm.float_samples[i] =
              float(.2 * std::sin(2 * std::numbers::pi * 440 * (block * 441 + i) / 44100));
        input.audio_source->write(pcm);  // Bounded queue; a busy input may drop this block.
      }
      engine->poll();
      if (auto failure = engine->next_failure())
        throw std::runtime_error(failure->message);
      std::this_thread::sleep_until(start + (block + 1) * 10ms);
    }
    engine->unpublish(id).get();
    engine->close().get();
    const auto stats = *operation.track->snapshot().audio;
    // Local codec completion is not a remote delivery acknowledgement.
    std::cout << "Encoded packets: " << stats.completed << ", dropped work items: " << stats.dropped
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
