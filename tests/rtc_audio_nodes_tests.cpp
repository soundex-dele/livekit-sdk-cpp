#include "rtc_audio_fixtures.hpp"
#include "livekit/rtc/stream_pipeline.hpp"
#include <condition_variable>
#include <iostream>
#include <limits>
#include <thread>
using namespace livekit::sdk::rtc;
using namespace std::chrono_literals;

namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

template <class F>
void eventually(F f, const char* message) {
  const auto end = SteadyClock::now() + 5s;
  while (!f()) {
    check(SteadyClock::now() < end, message);
    std::this_thread::sleep_for(1ms);
  }
}

class Input : public graphflow::core::GraphNode {
 public:
  Input() : GraphNode("input", nullptr) {}

  void raw(nodes::AudioFrameEvent frame) {
    publish(frame);
  }

  void encoded(EncodedFrame frame) {
    publish(nodes::EncodedFrameEvent(std::move(frame)));
  }

  void feedback(Epoch epoch, uint64_t revision = 1, bool active = true, uint64_t generation = 0) {
    SenderFeedback f;
    f.epoch = epoch;
    f.revision = revision;
    f.encodings[""] = {64000, active, false, 0};
    f.media_generation = generation;
    publish(nodes::SenderFeedbackEvent(f));
  }
};

class Output : public graphflow::core::GraphNode {
 public:
  Output() : GraphNode("output", nullptr) {}

  std::mutex mutex;
  std::condition_variable changed;
  bool block = false, entered = false;
  std::vector<EncodedFrame> packets;
  std::vector<nodes::AudioFrameEvent> frames;

  void unblock() {
    std::lock_guard lock(mutex);
    block = false;
    changed.notify_all();
  }

 protected:
  bool doInit() override {
    subscribe<nodes::EncodedFrameEvent>([this](const auto& event) {
      std::unique_lock lock(mutex);
      packets.push_back(event.frame);
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return !block; });
    });
    subscribe<nodes::AudioFrameEvent>([this](const auto& event) {
      std::lock_guard lock(mutex);
      frames.push_back(event);
    });
    return true;
  }
};

using G = engine::TrackGraphSpec;

void roundtrip(uint16_t channels, uint32_t rate, bool floating) {
  auto input = std::make_shared<Input>();
  auto output = std::make_shared<Output>();
  auto d = rtc_audio_test::descriptor("tone", channels);
  auto encoder = std::make_shared<nodes::AudioCodecNode>("encoder", d, true);
  auto decoder = std::make_shared<nodes::AudioCodecNode>("decoder", d, false);
  graphflow::engine::GraphEngine graph{1};
  check(graph.loadFromNodes({output, decoder, encoder, input},
                            {G::connect<nodes::AudioFrameEvent>(input, encoder),
                             G::connect<nodes::SenderFeedbackEvent>(input, encoder),
                             G::connect<nodes::EncodedFrameEvent>(encoder, decoder),
                             G::connect<nodes::EncodedFrameEvent>(encoder, output),
                             G::connect<nodes::AudioFrameEvent>(decoder, output)}) &&
            graph.init() && graph.start(),
        "Audio graph start failed");
  input->feedback(1);
  const uint64_t origin = 0xfffffe20ULL;  // Packet and PCM timing crosses RTP's 32-bit wrap.
  for (int packet = 0; packet < 12; ++packet) {
    for (int i = 0; i < 2; ++i)
      input->raw(
          rtc_audio_test::tone(1, origin + 480 * (packet * 2 + i), rate, channels, floating));
    eventually(
        [&] {
          std::lock_guard lock(output->mutex);
          return output->frames.size() == size_t(packet + 1);
        },
        "PCM did not round-trip through actual Opus nodes");
  }
  encoder->quiesce();
  decoder->quiesce();
  graph.stop();
  graph.release();
  encoder->throw_if_failed();
  decoder->throw_if_failed();
  std::lock_guard lock(output->mutex);
  check(output->packets.size() == 12, "Opus packet aggregation lost data");
  for (size_t i = 0; i < 12; ++i) {
    const auto& frame = output->frames[i];
    const auto& packet = output->packets[i];
    check(packet.timestamp == origin + i * 960 && packet.duration == 20ms && packet.epoch == 1,
          "Opus packet timing lost precision/wrap");
    check(frame.timestamp == packet.timestamp && frame.pcm->sample_rate == 48000 &&
              frame.pcm->channels == channels && frame.pcm->samples_per_channel() == 960 &&
              frame.discontinuity == (i == 0),
          "Decoded PCM format/continuity mismatch");
    if (i > 2)
      for (size_t ch = 0; ch < channels; ++ch) {
        const auto intended = rtc_audio_test::tone_power(*frame.pcm, ch, 440 * (1 + .5 * ch));
        const auto other = rtc_audio_test::tone_power(*frame.pcm, ch, 1100);
        check(intended > 1000000 && intended > other * 10,
              "PCM resampling/Opus changed tone or channel routing");
      }
  }
}

void jitter_loss_and_reordering() {
  auto source = std::make_shared<Input>();
  auto packets = std::make_shared<Output>();
  auto descriptor = rtc_audio_test::descriptor("jitter");
  auto encoder = std::make_shared<nodes::AudioCodecNode>("encoder", descriptor, true);
  graphflow::engine::GraphEngine encoding{1};
  check(encoding.loadFromNodes({packets, encoder, source},
                               {G::connect<nodes::AudioFrameEvent>(source, encoder),
                                G::connect<nodes::SenderFeedbackEvent>(source, encoder),
                                G::connect<nodes::EncodedFrameEvent>(encoder, packets)}) &&
            encoding.init() && encoding.start(),
        "Jitter fixture encoder failed");
  source->feedback(1);
  for (int packet = 0; packet < 16; ++packet) {
    source->raw(rtc_audio_test::tone(1, packet * 960, 48000, 1, false));
    source->raw(rtc_audio_test::tone(1, packet * 960 + 480, 48000, 1, false));
    eventually(
        [&] {
          std::lock_guard lock(packets->mutex);
          return packets->packets.size() == size_t(packet + 1);
        },
        "Opus fixture failed to produce a packet");
  }
  encoder->quiesce();
  encoding.stop();
  encoding.release();
  auto input = std::make_shared<Input>();
  auto output = std::make_shared<Output>();
  AudioCodecOptions options;
  options.jitter_buffer = true;
  auto decoder =
      std::make_shared<nodes::AudioCodecNode>("jitter.decoder", descriptor, false, options);
  graphflow::engine::GraphEngine graph{1};
  check(graph.loadFromNodes({output, decoder, input},
                            {G::connect<nodes::EncodedFrameEvent>(input, decoder),
                             G::connect<nodes::AudioFrameEvent>(decoder, output)}) &&
            graph.init() && graph.start(),
        "Jitter graph failed");
  const auto send = [&](int i, Epoch epoch = 1) {
    auto packet = packets->packets.at(size_t(i));
    packet.epoch = epoch;
    packet.sequence_number = uint16_t(65530 + i);
    packet.sender_capture_time_us = 1000000 + i * 20000;
    input->encoded(std::move(packet));
  };
  send(0);
  send(2);
  send(1);  // Packet reordering plus sequence-number wrap.
  for (int i = 3; i < 16; ++i) {
    if (i < 5 || i > 8) {
      send(i);  // Four genuinely missing Opus packets.
    }
    std::this_thread::sleep_for(20ms);
  }
  eventually([&] { return decoder->snapshot().concealed_samples > 0; },
             "Packet loss did not invoke PLC");
  {
    std::lock_guard lock(output->mutex);
    check(output->frames.size() >= 10, "Jitter decoder did not produce paced audio");
    for (const auto& frame : output->frames) {
      check(frame.pcm->samples_per_channel() == 480 && frame.pcm->sample_rate == 48000,
            "NetEq output is not a 10ms 48kHz block");
    }
  }
  send(0, 2);
  send(1, 2);
  eventually(
      [&] {
        std::lock_guard lock(output->mutex);
        return std::any_of(output->frames.begin(), output->frames.end(),
                           [](const auto& f) { return f.epoch == 2; });
      },
      "Jitter decoder did not reset after rejoin");
  decoder->quiesce();
  graph.stop();
  graph.release();
  decoder->throw_if_failed();
}

void overload_and_epoch() {
  auto in = std::make_shared<Input>();
  auto out = std::make_shared<Output>();
  out->block = true;
  AudioCodecOptions options;
  options.max_frames = 2;
  auto encoder = std::make_shared<nodes::AudioCodecNode>(
      "encoder", rtc_audio_test::descriptor("bounded"), true, options);
  graphflow::engine::GraphEngine graph{1};
  check(graph.loadFromNodes({out, encoder, in},
                            {G::connect<nodes::AudioFrameEvent>(in, encoder),
                             G::connect<nodes::SenderFeedbackEvent>(in, encoder),
                             G::connect<nodes::EncodedFrameEvent>(encoder, out)}) &&
            graph.init() && graph.start(),
        "Bounded audio graph start failed");

  struct Unblock {
    std::shared_ptr<Output> out;

    ~Unblock() {
      out->unblock();
    }
  } cleanup{out};

  in->feedback(1);
  in->raw(rtc_audio_test::tone(1, 0));
  in->raw(rtc_audio_test::tone(1, 480));
  eventually(
      [&] {
        std::lock_guard lock(out->mutex);
        return out->entered;
      },
      "Audio worker gate not reached");
  for (int i = 2; i < 50; ++i)
    in->raw(rtc_audio_test::tone(1, i * 480));
  check(encoder->snapshot().queued_frames <= 2 && encoder->snapshot().dropped > 0,
        "Audio worker queue is unbounded");
  in->feedback(1, 2, false, 1);
  check(encoder->snapshot().queued_frames == 0, "Audio mute retained queued PCM");
  const auto muted_admissions = encoder->snapshot().admitted;
  in->raw(rtc_audio_test::tone(1, 25000));
  check(encoder->snapshot().admitted == muted_admissions, "Inactive audio codec admitted PCM");
  in->feedback(1, 3, true, 2);
  in->raw(rtc_audio_test::tone(1, 26000));
  check(encoder->snapshot().queued_frames == 1, "Audio unmute did not resume admission");
  auto held = rtc_audio_test::tone(1, 28000);
  in->feedback(1, 4, true, 4);  // A complete mute/unmute occurred between engine polls.
  check(encoder->snapshot().queued_frames == 0, "Audio codec missed a mute boundary between polls");
  const auto admissions = encoder->snapshot().admitted;
  in->raw(held);
  check(encoder->snapshot().admitted == admissions, "PCM held across mute was retagged as fresh");
  in->feedback(2);
  check(encoder->snapshot().queued_frames == 0, "Epoch retained queued PCM");
  auto before = encoder->snapshot().dropped;
  in->raw(rtc_audio_test::tone(1, 30000));
  auto invalid = rtc_audio_test::tone(2, 0);
  auto pcm = std::make_shared<PcmAudioFrame>(*invalid.pcm);
  pcm->float_samples[0] = std::numeric_limits<float>::quiet_NaN();
  invalid.pcm = pcm;
  in->raw(invalid);
  check(encoder->snapshot().dropped == before + 2, "Stale or nonfinite PCM admitted");
  out->unblock();
  in->raw(rtc_audio_test::tone(2, 0));
  in->raw(rtc_audio_test::tone(2, 480));
  eventually(
      [&] {
        std::lock_guard lock(out->mutex);
        return out->packets.size() == 2;
      },
      "New epoch did not recover Opus");
  encoder->quiesce();
  graph.stop();
  encoder->throw_if_failed();
  std::lock_guard lock(out->mutex);
  check(out->packets.back().epoch == 2 && out->packets.back().timestamp == 0,
        "Old partial packet crossed epoch");
}

void decode_gaps() {
  auto in = std::make_shared<Input>();
  auto out = std::make_shared<Output>();
  auto decoder = std::make_shared<nodes::AudioCodecNode>(
      "decoder", rtc_audio_test::descriptor("receive"), false);
  graphflow::engine::GraphEngine graph{1};
  check(
      graph.loadFromNodes({out, decoder, in}, {G::connect<nodes::EncodedFrameEvent>(in, decoder),
                                               G::connect<nodes::AudioFrameEvent>(decoder, out)}) &&
          graph.init() && graph.start(),
      "Receive graph failed");
  const auto send = [&](Epoch epoch, uint64_t timestamp,
                        std::vector<uint8_t> data = {0xf8, 0xff, 0xfe}) {
    EncodedFrame f;
    f.buffer = EncodedBuffer(std::move(data));
    f.timestamp = timestamp;
    f.epoch = epoch;
    in->encoded(std::move(f));
  };
  const uint64_t timestamps[] = {0, 960, 3840};
  for (size_t i = 0; i < 3; ++i) {
    send(1, timestamps[i]);
    eventually(
        [&] {
          std::lock_guard lock(out->mutex);
          return out->frames.size() == i + 1;
        },
        "Opus decode stalled");
  }
  send(2, 0);
  eventually(
      [&] {
        std::lock_guard lock(out->mutex);
        return out->frames.size() == 4;
      },
      "Decoder epoch stalled");
  send(2, 960, {0x03});
  eventually([&] { return decoder->snapshot().invalid_packets > 0; },
             "Invalid Opus packet not reported");
  send(2, 1920);
  eventually(
      [&] {
        std::lock_guard lock(out->mutex);
        return out->frames.size() == 5;
      },
      "Decoder did not recover after malformed Opus");
  decoder->quiesce();
  graph.stop();
  graph.release();
  decoder->throw_if_failed();
  std::lock_guard lock(out->mutex);
  check(!out->frames[1].discontinuity && out->frames[2].discontinuity &&
            out->frames[2].timestamp == 3840 && out->frames[3].discontinuity &&
            out->frames[3].epoch == 2 && out->frames[4].discontinuity,
        "Decode hid a gap/epoch/reset");
}
}  // namespace

int main() {
  try {
    roundtrip(1, 44100, true);
    roundtrip(2, 48000, false);
    overload_and_epoch();
    jitter_loss_and_reordering();
    decode_gaps();
    std::cout << "Audio graph codec contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
