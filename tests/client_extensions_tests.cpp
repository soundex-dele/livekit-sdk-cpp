#include <livekit/livekit.hpp>
#include "rtc_services_contract.hpp"
#include "backends/rtc/stream_session.hpp"
#include "backends/rtc/data_track_session.hpp"
#include "backends/rtc/encryption.hpp"
#include "media_nodes/capture/encoded_source.hpp"
#include "media_nodes/capture/audio_source.hpp"
#include "media_nodes/capture/video_source.hpp"
#include "support/control_executor.hpp"
#include <algorithm>
#include <iostream>
#include <thread>

using namespace livekit::sdk;
using namespace livekit::sdk::detail;
using namespace std::chrono_literals;

static void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

template <class F>
static void rejects(F fn) {
  bool caught = false;
  try {
    fn();
  } catch (const std::exception&) {
    caught = true;
  }
  require(caught, "Expected rejection");
}

static std::span<const uint8_t> bytes(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

static void encoded_and_rtp() {
  EncodedSourceOptions options;
  options.codec = "h264";
  auto source = std::make_shared<EncodedMediaSource>(options);
  EncodedSourceReader reader(source);
  reader.start();
  EncodedMediaFrame frame;
  frame.data = {0, 0, 0, 1, 0x65, 0x88};
  frame.keyframe = true;
  require(source->write(frame) == EncodedWriteStatus::accepted, "Encoded input");
  auto copy = reader.read({}, false);
  require(copy && copy->data == frame.data, "Encoded ownership");
  reader.feedback(true);
  frame.keyframe = false;
  frame.data[4] = 0x41;
  require(source->write(frame) == EncodedWriteStatus::keyframe_required, "Keyframe recovery");
  rejects([&] { EncodedSourceReader duplicate(source); });
  reader.stop();
  EncodedSourceOptions opus;
  opus.codec = "opus";
  RtpMediaSource rtp(opus, 111, 1);
  EncodedSourceReader rtp_reader(rtp.source());
  rtp_reader.start();
  std::vector<uint8_t> packet = {0x80, 0xef, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 1, 0xf8, 0xff, 0xfe};
  require(rtp.write(packet) == EncodedWriteStatus::accepted, "RTP depacketization");
  require(rtp_reader.read({}, false)->data == std::vector<uint8_t>({0xf8, 0xff, 0xfe}),
          "RTP payload preserved");
  packet[2] = packet[3] = 0;
  packet[7] = 0x60;
  require(rtp.write(packet) == EncodedWriteStatus::accepted, "RTP sequence wrap");
  packet[8] = 1;
  rejects([&] { rtp.write(packet); });
  RtpClock clock(90000);
  require(!clock.ntp_time(0), "Unsynchronized RTP clock");
  clock.sender_report(0xffff0000, 10ull << 32);
  require(clock.ntp_time(uint32_t(0xffff0000u + 90000u)) == 11s, "RTP timestamp wrap");
  clock.reset();
  require(!clock.ntp_time(0), "RTP clock reset");
  auto stream = std::make_shared<EncodedMediaStream>(1, 65536);
  EncodedOutput output(stream);
  auto generation = output.advance();
  output.push(frame, generation);
  output.advance();
  require(stream->stats().queued_frames == 0, "Output generation barrier");
  output.push(frame, generation);
  require(stream->stats().queued_frames == 0, "Old output rejected");
  output.close();
  require(!stream->read(0ms), "Encoded stream closes");
}

static void partial_pcm_and_callbacks() {
  I420SourceOptions videoOptions;
  videoOptions.width = 16;
  videoOptions.height = 16;
  videoOptions.dynamic_dimensions = true;
  videoOptions.max_bytes = 2048;
  auto video = std::make_shared<I420VideoSource>(videoOptions);
  I420SourceReader videoReader(video);
  videoReader.start();
  RgbaInputFrame rgba;
  rgba.width = 32;
  rgba.height = 16;
  rgba.stride = 128;
  rgba.pixels.assign(2048, 100);
  rgba.rotation = VideoRotation::clockwise90;
  require(video->write_rgba(rgba) == VideoWriteStatus::accepted, "Dynamic rotated RGBA accepted");
  auto resized = videoReader.read({});
  require(resized && resized->frame.width == 16 && resized->frame.height == 32 &&
              video->stats().queued_bytes == 0,
          "Dynamic dimensions and actual queue accounting");
  rgba.width = 64;
  rgba.height = 32;
  rgba.stride = 256;
  rgba.pixels.resize(8192);
  require(video->write_rgba(rgba) == VideoWriteStatus::backpressure,
          "Dynamic resolution obeys byte limit");
  videoReader.stop();
  PcmSourceOptions options;
  options.sample_rate = 8000;
  options.channels = 1;
  auto source = std::make_shared<PcmAudioSource>(options);
  PcmSourceReader reader(source);
  reader.start();
  PcmInputFrame a;
  a.samples.assign(23, 123);
  PcmInputFrame b;
  b.samples.assign(57, 456);
  require(source->write(a) == AudioWriteStatus::accepted && source->stats().queued_blocks == 0,
          "Partial PCM buffered");
  require(source->write(b) == AudioWriteStatus::accepted, "Partial PCM completed");
  auto block = reader.read();
  require(block && block->samples.size() == 80 && block->samples[22] == 123 &&
              block->samples[23] == 456,
          "PCM sample continuity");
  PcmSourceOptions driftOptions;
  driftOptions.drift_compensation = true;
  auto drift = std::make_shared<PcmAudioSource>(driftOptions);
  PcmSourceReader driftReader(drift);
  driftReader.start();
  const auto epoch = std::chrono::steady_clock::now() - 1s;
  PcmInputFrame driftFrame;
  driftFrame.samples.assign(480, 100);
  for (int i = 0; i < 100; ++i) {
    driftFrame.capture_time = epoch + std::chrono::microseconds(i * 10005);
    require(drift->write(driftFrame) == AudioWriteStatus::accepted, "Drift input accepted");
    driftReader.read();
  }
  const auto driftStats = drift->stats();
  require(driftStats.estimated_drift_ppm > 400 && driftStats.estimated_drift_ppm <= 1000 &&
              driftStats.corrected_samples > 10,
          "Bounded PCM device clock correction");
  std::vector<int> values;
  ControlExecutor executor(32, [] {});
  for (int i = 0; i < 20; ++i)
    executor.post([&, i] { values.push_back(i); });
  executor.drain();
  require(values.size() == 20 && std::is_sorted(values.begin(), values.end()),
          "Ordered callback barrier");
  bool failed = false;
  ControlExecutor failing(4, [&] { failed = true; });
  failing.post([] { throw std::runtime_error("callback"); });
  rejects([&] { failing.drain(); });
  require(failed, "Callback exception stops session");
}

int main() {
  try {
    std::cerr << "streams\n";
    rtc_services_contract::streams();
    std::cerr << "encryption\n";
    rtc_services_contract::encryption();
    std::cerr << "data tracks plain\n";
    rtc_services_contract::data_tracks(false);
    std::cerr << "data tracks encrypted\n";
    rtc_services_contract::data_tracks(true);
    std::cerr << "encoded and RTP\n";
    encoded_and_rtp();
    std::cerr << "PCM and callbacks\n";
    partial_pcm_and_callbacks();
    std::cout << "Client extension contracts passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
