#include "livekit/compat/algorithm.hpp"
#include "../media_result.hpp"
using livekit::sdk::examples::report_result;
#include <livekit/media_engine.hpp>
#include <cmath>
#include <iostream>
#include <thread>

using namespace livekit::sdk::client;
using namespace std::chrono_literals;

int main(int argc, char** argv) {
  if (argc != 3 && (argc != 4 || std::string(argv[3]) != "--microphone")) {
    std::cerr << "Usage: audio_source <server-url> <token> [--microphone]\n";
    return 2;
  }

  MediaEngine engine;
  ConnectionOptions options;
  options.url = argv[1];
  options.token = argv[2];
  if (!engine.connect(options)) {
    return 1;
  }
  MediaPublishSpec config;
  config.name = "application-audio";
  config.kind = MediaKind::audio;
  config.format.codec = Codec::opus;
  config.format.clock_rate = 48000;
  config.format.channels = 1;
  MediaInput input;
  if (argc == 4) {
    input.microphone = livekit::sdk::MicrophoneOptions{};
    config.options.source = PublicationSource::microphone;
  } else {
    livekit::sdk::PcmSourceOptions pcm;
    pcm.sample_rate = 44100;
    pcm.format = livekit::sdk::AudioSampleFormat::float32;
    input.audio_source = std::make_shared<livekit::sdk::PcmAudioSource>(pcm);
  }
  config.input = input;
  auto operation_result = engine.publish_media(std::move(config));
  if (!report_result(operation_result)) {
    return 1;
  }
  auto operation = *std::move(operation_result);
  const auto id = operation.publication.snapshot().track_id;
  const auto start = SteadyClock::now();
  for (uint64_t block = 0; block < 1000; ++block) {
    if (input.audio_source) {
      livekit::sdk::PcmInputFrame pcm;
      pcm.float_samples.resize(441);
      for (size_t i = 0; i < 441; ++i) {
        pcm.float_samples[i] =
            float(.2 * std::sin(2 * livekit::compat::pi * 440 * (block * 441 + i) / 44100));
      }
      if (!report_result(input.audio_source->write(pcm))) {
        return 1;  // Bounded queue; a busy input may drop this block.
      }
    }

    if (engine.connection_state() == ConnectionState::failed ||
        engine.connection_state() == ConnectionState::closed) {
      return 1;
    }
    if (auto failure = operation.track->snapshot().failure) {
      std::cerr << failure->message << '\n';
      return 1;
    }
    std::this_thread::sleep_until(start + (block + 1) * 10ms);
  }
  engine.unpublish_track(id);
  engine.close();
  std::cout << "Submitted encoded access units: " << operation.track->snapshot().frames << '\n';
}
