#include "rtc_blocking_bridge.hpp"
#include "../media_result.hpp"
using livekit::sdk::examples::report_result;
#include "rtc_event_mailbox.hpp"
#include "livekit/rtc/livekit.hpp"
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

  auto engine_result =
      livekit::sdk::app_support::create_blocking_engine(livekit::sdk::rtc::AudioOutputOptions{});
  if (!report_result(engine_result)) {
    return 1;
  }
  auto engine = *std::move(engine_result);
  livekit::sdk::app_support::RtcEventMailbox engine_events(*engine);
  if (!report_result(engine_events.status())) {
    return 1;
  }
  LiveKitRoomOptions options;
  options.url = argv[1];
  options.token = argv[2];
  if (!report_result(engine->join(options))) {
    return 1;
  }
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
  auto operation_result = engine->publish_audio(std::move(config));
  if (!report_result(operation_result)) {
    return 1;
  }
  auto operation = *std::move(operation_result);
  const auto id = operation.id;
  const auto start = SteadyClock::now();
  for (uint64_t block = 0; block < 1000; ++block) {
    if (input.audio_source) {
      livekit::sdk::PcmInputFrame pcm;
      pcm.float_samples.resize(441);
      for (size_t i = 0; i < 441; ++i) {
        pcm.float_samples[i] =
            float(.2 * std::sin(2 * std::numbers::pi * 440 * (block * 441 + i) / 44100));
      }
      if (!report_result(input.audio_source->write(pcm))) {
        return 1;  // Bounded queue; a busy input may drop this block.
      }
    }

    auto failure_result = engine_events.take_failure();
    if (!report_result(failure_result)) {
      return 1;
    }
    if (auto failure = *std::move(failure_result)) {
      std::cerr << failure->message << "\n";
      return 1;
    }
    std::this_thread::sleep_until(start + (block + 1) * 10ms);
  }
  if (!report_result(engine->unpublish(id))) {
    return 1;
  }
  if (!report_result(engine->close())) {
    return 1;
  }
  const auto stats = *operation.track->snapshot().audio;
  // Local codec completion is not a remote delivery acknowledgement.
  std::cout << "Encoded packets: " << stats.completed << ", dropped work items: " << stats.dropped
            << '\n';
}
