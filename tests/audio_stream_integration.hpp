#pragma once
#include <livekit/livekit.hpp>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <thread>

namespace audio_stream_test {
using namespace livekit::sdk;
using namespace std::chrono_literals;
inline void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void until(F predicate, const char* message) {
  const auto end = std::chrono::steady_clock::now() + 15s;
  while (!predicate()) { check(std::chrono::steady_clock::now() < end, message); std::this_thread::sleep_for(10ms); }
}
struct Tone {
  uint64_t count = 0, crossings = 0, last_sequence = 0, timeline = 0;
  double peak = 0;
  std::vector<int16_t> captured;
  int previous = 0, rate = 0;
  void add(const AudioFrame& frame) {
    check(frame.format == AudioSampleFormat::pcm16 && frame.samples.size() == frame.samples_per_channel * frame.channels && frame.float_samples.empty(), "PCM block layout");
    check(frame.sample_rate > 0 && frame.channels > 0 && frame.duration() == 10ms, "Native audio format/duration");
    if (frame.track_generation != timeline || frame.sequence != last_sequence + 1) previous = 0;
    timeline = frame.track_generation; last_sequence = frame.sequence; rate = frame.sample_rate;
    for (size_t i = 0; i < frame.samples_per_channel; ++i) {
      const auto value = frame.samples[i * frame.channels];
      if (previous < 0 && value >= 0) ++crossings;
      previous = value; peak = std::max(peak, std::abs(static_cast<double>(value))); ++count;
      captured.push_back(value);
    }
  }
  double frequency() const { return static_cast<double>(crossings) * rate / count; }
};
inline int run(const char* server, const char* proxy, const char* directory) {
  auto token = [](const char* identity) {
    return make_token("reconnect-test", "reconnect-integration-secret-32-characters", identity,
      {{"roomJoin", true}, {"room", "audio-stream"}, {"canPublish", true}, {"canSubscribe", true},
       {"canPublishData", true}, {"canUpdateOwnMetadata", true}});
  };
  auto stream = std::make_shared<AudioFrameStream>(AudioStreamOptions{20, 80 * 1024});
  std::atomic<int> rejoins{0};
  RoomCallback callbacks;
  callbacks.on_reconnecting = [&](const ReconnectInfo&) {
    check(stream->stats().queued_frames == 0 && stream->stats().generation > 1, "Old audio remained at reconnect boundary");
  };
  callbacks.on_reconnected = [&](const ReconnectInfo& info) { if (info.mode == ReconnectMode::rejoin) ++rejoins; };
  Room reader(callbacks), publisher, float_reader;
  RoomOptions options; options.signal_protocol = 15; options.audio_frame_stream = stream;
  reader.join_with_token(proxy, token("pcm16-reader"), options);
  auto floats = std::make_shared<AudioFrameStream>(AudioStreamOptions{10, 80 * 1024, AudioSampleFormat::float32});
  RoomOptions float_options; float_options.audio_frame_stream = floats;
  float_reader.join_with_token(server, token("float-reader"), float_options);
  publisher.join_with_token(server, token("publisher"));
  MediaSpec media; media.video = false; media.loop = true;
  media.input = (std::filesystem::path(directory) / "440.wav").string();
  auto first = publisher.local_participant().publish_track(media, {"tone-440"}).get();
  media.input = (std::filesystem::path(directory) / "880.wav").string();
  auto second = publisher.local_participant().publish_track(media, {"tone-880"}).get();
  until([&] { return stream->stats().received >= 60 && stream->stats().tracks == 2; }, "Independent audio tracks did not arrive");
  auto full = stream->stats();
  check(full.queued_frames <= 20 && full.queued_bytes <= 80 * 1024 && full.dropped > 0, "Slow PCM reader exceeded capacity");
  auto metadata = reader.local_participant().set_metadata("audio-reader-not-consuming", OperationOptions{2s, {}, true});
  check(metadata.wait_for(3s) == std::future_status::ready, "Slow audio reader blocked signaling"); metadata.get();
  auto selected = stream->read_for(first.sid(), 2s);
  check(selected.status == AudioReadStatus::frame && selected.frame->track_id == first.sid() && selected.frame->sample_index > 0, "Per-track selection or dropped sample position missing");
  auto retained = selected.frame;
  auto old_samples = retained->samples;
  // NetEq drives sinks with silence before the first decoded packet. Start
  // frequency measurement only after both tracks actually carry their tone;
  // keep all subsequent samples (including silence/gaps) in the measurement.
  std::set<std::string> audible_tracks;
  size_t warmup_blocks = 0;
  const auto warmup_end = std::chrono::steady_clock::now() + 10s;
  while (audible_tracks.size() < 2) {
    check(std::chrono::steady_clock::now() < warmup_end, "Audio tracks never produced audible PCM");
    auto value = stream->read(1s);
    check(value.status == AudioReadStatus::frame, "PCM warmup stopped");
    ++warmup_blocks;
    if (std::any_of(value.frame->samples.begin(), value.frame->samples.end(), [](int16_t sample) { return std::abs(static_cast<int>(sample)) > 500; }))
      audible_tracks.insert(value.frame->track_id);
  }
  std::map<std::string, Tone> tones;
  const auto end = std::chrono::steady_clock::now() + 10s;
  while (tones[first.sid()].count < 48000 || tones[second.sid()].count < 48000) {
    check(std::chrono::steady_clock::now() < end, "PCM frequency collection timed out");
    auto value = stream->read(1s);
    check(value.status == AudioReadStatus::frame, "PCM reader stopped");
    check(value.frame->track_id == first.sid() || value.frame->track_id == second.sid(), "Unexpected PCM track");
    tones[value.frame->track_id].add(*value.frame);
  }
  const double low = tones[first.sid()].frequency(), high = tones[second.sid()].frequency();
  Json diagnostics = Json::array();
  for (const auto& [sid, tone] : tones) {
    std::ofstream pcm(std::filesystem::path(directory) / (sid + ".pcm"), std::ios::binary);
    pcm.write(reinterpret_cast<const char*>(tone.captured.data()), tone.captured.size() * sizeof(int16_t));
    diagnostics.push_back({{"sid", sid}, {"rate", tone.rate}, {"samples", tone.count}, {"frequency", tone.frequency()}, {"peak", tone.peak}});
  }
  std::ofstream(std::filesystem::path(directory) / "pcm-diagnostics.json") << diagnostics.dump(2);
  check(std::abs(low - 440) < 22 && std::abs(high - 880) < 44 && tones[first.sid()].peak > 500 && tones[second.sid()].peak > 500,
      "PCM tracks were mixed, silent, or had incorrect sample timing");
  bool float_audible = false;
  for (int i = 0; i < 30 && !float_audible; ++i) {
    auto value = floats->read_for(second.sid(), 1s);
    check(value.status == AudioReadStatus::frame && value.frame->format == AudioSampleFormat::float32 && value.frame->samples.empty(), "Float PCM output missing");
    for (float sample : value.frame->float_samples) { check(std::isfinite(sample) && sample >= -1 && sample < 1, "Unnormalized float PCM"); float_audible |= std::abs(sample) > .02f; }
  }
  check(float_audible, "Float PCM output was silent");
  std::ofstream(std::filesystem::path(directory) / "audio-rejoin") << "ready";
  until([&] { return rejoins == 1; }, "Audio output rejoin failed");
  auto recovered = stream->read_for(first.sid(), 5s);
  check(recovered.status == AudioReadStatus::frame && recovered.frame->stream_generation > retained->stream_generation,
      "Audio rejoin leaked old generation");
  const auto float_before = floats->stats().received;
  reader.disconnect(); reader.wait();
  check(stream->read(0ms).status == AudioReadStatus::closed && stream->stats().queued_bytes == 0 && retained->samples == old_samples,
      "Closing PCM stream invalidated owned data or retained queued blocks");
  until([&] { return floats->stats().received >= float_before + 20; }, "Closing one Room stopped another audio stream");
  publisher.local_participant().unpublish_track(first.sid()).get();
  publisher.local_participant().unpublish_track(second.sid()).get();
  until([&] { return floats->stats().tracks == 0 && floats->stats().queued_frames == 0; }, "Track removal left queued PCM");
  float_reader.disconnect(); float_reader.wait(); publisher.disconnect(); publisher.wait();
  std::cout << Json{{"ok", true}, {"frequency440", low}, {"frequency880", high}, {"warmupBlocks", warmup_blocks}, {"pcm16", true}, {"float32", true},
      {"independentTracks", true}, {"independentRooms", true}, {"slowReaderDoesNotBlock", true}, {"rejoins", rejoins.load()},
      {"received", stream->stats().received}, {"dropped", stream->stats().dropped}, {"generationReset", true}, {"ownedSamplesSurviveClose", true}}.dump() << '\n';
  return 0;
}
}
