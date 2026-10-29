#pragma once
#include "media_nodes/capture/audio_source.hpp"
#include <future>
#include <limits>

inline void audio_source_contract() {
  using namespace livekit::sdk;
  using namespace std::chrono_literals;
  auto require = [](bool ok) {
    if (!ok)
      throw std::runtime_error("PCM source contract failed");
  };
  auto reject = [&](auto action) {
    bool rejected = false;
    try {
      action();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    require(rejected);
  };
  auto source = std::make_shared<PcmAudioSource>(
      PcmSourceOptions{16000, 2, AudioSampleFormat::pcm16, 20ms, 1280});
  PcmInputFrame frame;
  frame.samples.resize(320);
  for (size_t i = 0; i < 160; ++i) {
    frame.samples[i * 2] = -1000;
    frame.samples[i * 2 + 1] = 2000;
  }
  require(source->write(frame) == AudioWriteStatus::backpressure);
  {
    detail::PcmSourceReader reader(source);
    reader.start();
    reject([&] { detail::PcmSourceReader duplicate(source); });
    const auto first_time = std::chrono::steady_clock::now();
    frame.capture_time = first_time;
    require(source->write(frame) == AudioWriteStatus::accepted);
    reject([&] { source->write(frame); });
    auto owned = reader.read();
    require(owned && owned->captured == first_time && owned->samples == frame.samples);
    frame.samples[0] = 123;
    require(owned->samples[0] == -1000);
    frame.capture_time = first_time + 30ms;
    require(source->write(frame) == AudioWriteStatus::accepted);
    require(reader.read()->silence && reader.read()->silence);
    require(!reader.read()->silence && source->stats().silence_samples == 320);
    frame.capture_time.reset();
    require(source->write(frame) == AudioWriteStatus::accepted);
    PcmInputFrame twice;
    twice.samples.resize(640, 111);
    const auto accepted = source->stats().accepted_samples;
    require(source->write(twice) == AudioWriteStatus::backpressure &&
            source->stats().accepted_samples == accepted && source->stats().queued_blocks == 1);
    require(source->write(frame) == AudioWriteStatus::accepted &&
            source->stats().queued_bytes == 1280);
    reader.skip(2);
    require(source->stats().queued_blocks == 0 && source->stats().dropped_samples == 320 &&
            source->stats().skipped_samples == 320);
    reader.stop();
    reader.start();
    std::stop_source cancel;
    auto waiting = std::async(std::launch::async, [&] { return reader.read(cancel.get_token()); });
    require(waiting.wait_for(20ms) == std::future_status::timeout);
    cancel.request_stop();
    require(waiting.wait_for(1s) == std::future_status::ready && !waiting.get());
    auto closing = std::async(std::launch::async, [&] { return reader.read(); });
    reader.stop();
    require(closing.wait_for(1s) == std::future_status::ready && !closing.get());
    require(source->stats().generation == 2);
  }
  require(!source->stats().attached && !source->stats().accepting);
  {
    detail::PcmSourceReader rebound(source);
    rebound.start();
    require(source->write(frame) == AudioWriteStatus::accepted && source->stats().generation == 3);
    source->close();
    require(source->stats().queued_bytes == 0 && !rebound.read() &&
            source->write(frame) == AudioWriteStatus::closed);
  }
  reject([&] { detail::PcmSourceReader closed(source); });
  auto floats = std::make_shared<PcmAudioSource>(
      PcmSourceOptions{8000, 1, AudioSampleFormat::float32, 100ms, 160});
  detail::PcmSourceReader float_reader(floats);
  float_reader.start();
  PcmInputFrame fp;
  fp.float_samples.resize(80, .5f);
  fp.float_samples[0] = -1;
  fp.float_samples[1] = 1;
  require(floats->write(fp) == AudioWriteStatus::accepted &&
          floats->write(fp) == AudioWriteStatus::backpressure);
  auto converted = float_reader.read();
  require(converted && converted->samples[0] == -32768 && converted->samples[1] == 32767 &&
          converted->samples[2] == 16384);
  fp.float_samples[0] = std::numeric_limits<float>::quiet_NaN();
  reject([&] { floats->write(fp); });
  fp.float_samples[0] = 1.1f;
  reject([&] { floats->write(fp); });
  fp.float_samples.pop_back();
  reject([&] { floats->write(fp); });
  fp.float_samples.assign(80, 0);
  fp.capture_time = std::chrono::steady_clock::now() + 1h;
  reject([&] { floats->write(fp); });
}
